/* -*-  Mode:C; c-basic-offset:4; tab-width:4 -*-
 ****************************************************************************
 * (c) 2004 - Rolf Neugebauer - Intel Research Cambridge
 * (c) 2004 - Keir Fraser - University of Cambridge
 ****************************************************************************
 * 
 * Description: allows a domain to access devices on the PCI bus
 *
 * A guest OS may be given access to particular devices on the PCI bus.
 * For each domain a list of PCI devices is maintained, describing the
 * access mode for the domain. 
 *
 * Guests can figure out the virtualised PCI space through normal PCI config
 * register access. Some of the accesses, in particular write accesses, are
 * faked. For example the sequence for detecting the IO regions, which requires
 * writes to determine the size of the region, is faked out by a very simple
 * state machine, preventing direct writes to the PCI config registers by a
 * guest.
 */

#include <xen/config.h>
#include <xen/lib.h>
#include <xen/types.h>
#include <xen/sched.h>
#include <xen/pci.h>
#include <xen/irq.h>
#include <xen/event.h>
#include <asm/pci.h>
#include <hypervisor-ifs/hypervisor-if.h>
#include <hypervisor-ifs/physdev.h>

/* Called by PHYSDEV_PCI_INITIALISE_DEVICE to finalise IRQ routing. */
extern void pcibios_enable_irq(struct pci_dev *dev);

#if 0
#define VERBOSE_INFO(_f, _a...) printk( _f , ## _a )
#else
#define VERBOSE_INFO(_f, _a...) ((void)0)
#endif

#if 1 || !defined(NDEBUG)
#define INFO(_f, _a...) printk( _f, ## _a )
#else
#define INFO(_f, _a...) ((void)0)
#endif


#define ACC_READ  1
#define ACC_WRITE 2

/* Upper bounds for PCI-device addressing. */
#define PCI_BUSMAX  255
#define PCI_DEVMAX   31
#define PCI_FUNCMAX   7
#define PCI_REGMAX  255

/* Bit offsets into state. */
#define ST_BASE_ADDRESS  0   /* bits 0-5: are for base address access */
#define ST_ROM_ADDRESS   6   /* bit 6: is for rom address access */    

typedef struct _phys_dev_st {
    int flags;                       /* flags for access etc */
    struct pci_dev *dev;             /* the device */
    struct list_head node;           /* link to the list */
    struct task_struct *owner;       /* 'owner of this device' */
    int state;                       /* state for various checks */
} phys_dev_t;


/* Find a device on a per-domain device list. */
static phys_dev_t *find_pdev(struct task_struct *p, struct pci_dev *dev)
{
    phys_dev_t *t, *res = NULL;
    struct list_head *tmp;

    list_for_each(tmp, &p->pcidev_list)
    {
        t = list_entry(tmp,  phys_dev_t, node);
        if ( dev == t->dev )
        {
            res = t;
            break;
        }
    }
    return res;
}

/* Add a device to a per-domain device-access list. */
static void add_dev_to_task(struct task_struct *p, 
                            struct pci_dev *dev, int acc)
{
    phys_dev_t *pdev;
    
    if ( (pdev = find_pdev(p, dev)) )
    {
        /* Sevice already on list: update access permissions. */
        pdev->flags = acc;
        return;
    }

    if ( !(pdev = kmalloc(sizeof(phys_dev_t), GFP_KERNEL)) )
    {
        INFO("Error allocating pdev structure.\n");
        return;
    }
    
    pdev->dev = dev;
    pdev->flags = acc;
    pdev->state = 0;
    list_add(&pdev->node, &p->pcidev_list);

    if ( acc == ACC_WRITE )
        pdev->owner = p;
}

/*
 * physdev_pci_access_modify:
 * Allow/disallow access to a specific PCI device.  Guests should not be
 * allowed to see bridge devices as it needlessly complicates things (one
 * possible exception to this is the AGP bridge).  If the given device is a
 * bridge, then the domain should get access to all the leaf devices below
 * that bridge (XXX this is unimplemented!).
 */
int physdev_pci_access_modify(
    domid_t dom, int bus, int dev, int func, int enable)
{
    struct task_struct *p;
    struct pci_dev *pdev;
    int i, j, rc = 0;
 
    if ( !IS_PRIV(current) )
        BUG();

    if ( (bus > PCI_BUSMAX) || (dev > PCI_DEVMAX) || (func > PCI_FUNCMAX) )
        return -EINVAL;

    if ( !enable )
    {
        INFO("Disallowing access is not yet supported.\n");
        return -EINVAL;
    }

    INFO("physdev_pci_access_modify: %02x:%02x:%02x\n", bus, dev, func);

    if ( (p = find_domain_by_id(dom)) == NULL ) 
        return -ESRCH;

    /* Make the domain privileged. */
    set_bit(PF_PHYSDEV, &p->flags);

    /* Grant write access to the specified device. */
    if ( (pdev = pci_find_slot(bus, PCI_DEVFN(dev, func))) == NULL )
    {
        INFO("  dev does not exist\n");
        rc = -ENODEV;
        goto out;
    }
    add_dev_to_task(p, pdev, ACC_WRITE);

    INFO("  add RW %02x:%02x:%02x\n", pdev->bus->number,
         PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));

    /* Is the device a bridge or cardbus? */
    if ( pdev->hdr_type != PCI_HEADER_TYPE_NORMAL )
        INFO("XXX can't give access to bridge devices yet\n");

    /* Now, setup access to the IO ports and memory regions for the device. */

    if ( p->io_bitmap == NULL )
    {
        p->io_bitmap = kmalloc(IO_BITMAP_BYTES, GFP_KERNEL);
        if ( p->io_bitmap == NULL )
        {
            rc = -ENOMEM;
            goto out;
        }
        memset(p->io_bitmap, 0xFF, IO_BITMAP_BYTES);

        p->io_bitmap_sel = ~0ULL;
    }

    for ( i = 0; i < DEVICE_COUNT_RESOURCE; i++ )
    {
        struct resource *r = &pdev->resource[i];
        
        if ( r->flags & IORESOURCE_IO )
        {
            /* Give the domain access to the IO ports it needs.  Currently,
             * this will allow all processes in that domain access to those
             * ports as well.  This will do for now, since driver domains don't
             * run untrusted processes! */
            INFO("Giving domain %llu IO resources (%lx - %lx) "
                 "for device %s\n", dom, r->start, r->end, pdev->slot_name);
            for ( j = r->start; j < r->end + 1; j++ )
            {
                clear_bit(j, p->io_bitmap);
                /* Record that we cleared a bit using bit n of the selector:
                 * n = (j / (4 bytes in a word * 8 bits in a byte))
                 *     / number of words per selector bit
                 */
                clear_bit((j / (8 * 4)) / IOBMP_SELBIT_LWORDS,
                          &p->io_bitmap_sel);
            }
        }

        /* rights to IO memory regions are checked when the domain maps them */
	}
 out:
    put_task_struct(p);
    return rc;
}

/* Check if a domain controls a device with IO memory within frame @pfn.
 * Returns: 1 if the domain should be allowed to map @pfn, 0 otherwise.  */
int domain_iomem_in_pfn(struct task_struct *p, unsigned long pfn)
{
    int ret = 0;
    struct list_head *l;

    VERBOSE_INFO("Checking if physdev-capable domain %llu needs access to "
                 "pfn %08lx\n", p->domain, pfn);
    
    spin_lock(&p->pcidev_lock);

    list_for_each(l, &p->pcidev_list)
    {
        int i;
        phys_dev_t *phys_dev = list_entry(l, phys_dev_t, node);
        struct pci_dev *pci_dev = phys_dev->dev;

        for ( i = 0; (i < DEVICE_COUNT_RESOURCE) && (ret == 0); i++ )
        {
            struct resource *r = &pci_dev->resource[i];
            
            if ( r->flags & IORESOURCE_MEM )
                if ( (r->start >> PAGE_SHIFT) == pfn
                     || (r->end >> PAGE_SHIFT) == pfn
                     || ((r->start >> PAGE_SHIFT < pfn)
                         && (r->end >> PAGE_SHIFT > pfn)) )
                    ret = 1;
        }

        if ( ret != 0 ) break;
    }
    
    spin_unlock(&p->pcidev_lock);

    VERBOSE_INFO("Domain %llu %s mapping of pfn %08lx\n",
                 p->domain, ret ? "allowed" : "disallowed", pfn);

    return ret;
}

/* check if a domain has general access to a device */
inline static int check_dev_acc (struct task_struct *p,
                                 int bus, int dev, int func,
                                 phys_dev_t **pdev) 
{
    struct pci_dev *target_dev;
    phys_dev_t     *target_pdev;
    unsigned int    target_devfn;

    *pdev = NULL;

     if ( !IS_CAPABLE_PHYSDEV(p) )
         return -EPERM; /* no pci access permission */

    if ( bus > PCI_BUSMAX || dev > PCI_DEVMAX || func > PCI_FUNCMAX )
        return -EINVAL;

    VERBOSE_INFO("b=%x d=%x f=%x ", bus, dev, func);

    /* check target device */
    target_devfn = PCI_DEVFN(dev, func);
    target_dev   = pci_find_slot(bus, target_devfn);
    if ( !target_dev )
    {
        VERBOSE_INFO("target does not exist\n");
        return -ENODEV;
    }

    /* check access */
    target_pdev = find_pdev(p, target_dev);
    if ( !target_pdev )
    {
        VERBOSE_INFO("dom has no access to target\n");
        return -EPERM;
    }

    *pdev = target_pdev;
    return 0;
}


/*
 * Base address registers contain the base address for IO regions.
 * The length can be determined by writing all 1s to the register and
 * reading the value again. The device will zero the lower unused bits.
 * 
 * to work out the length of the io region a device probe typically does:
 * 1) a = read_base_addr_reg()
 * 2) write_base_addr_reg(0xffffffff)
 * 3) b = read_base_addr_reg() // device zeros lower bits
 * 4) write_base_addr_reg(a) // restore original value
 * this function fakes out step 2-4. *no* writes are made to the device.
 * 
 * phys_dev_t contains a bit field (a bit for each base address register).
 * if the bit for a register is set the guest had writen all 1s to the 
 * register and subsequent read request need to fake out the b.
 * if the guest restores the original value (step 4 above) the bit is
 * cleared again. If the guest attempts to "restores" a wrong value an
 * error is flagged.
 */
static int do_base_address_access(phys_dev_t *pdev, int acc, int idx, 
                                  int len, u32 *val)
{
    int st_bit, reg = PCI_BASE_ADDRESS_0 + (idx*4), ret = -EINVAL;
    struct pci_dev *dev = pdev->dev;
    u32 orig_val, sz;
    struct resource *res;

    if ( len != sizeof(u32) )
    {
        /* This isn't illegal, but there doesn't seem to be a very good reason
         * to do it for normal devices (bridges are another matter).  Since it
         * would complicate the code below, we don't support this for now. */

        /* We could set *val to some value but the guest may well be in trouble
         * anyway if this write fails.  Hopefully the printk will give us a
         * clue what went wrong. */
        printk("Guest %llu attempting sub-dword %s to BASE_ADDRESS %d\n",
               pdev->owner->domain, (acc == ACC_READ) ? "read" : "write", idx);
        
        return -EPERM;
    }

    st_bit = idx + ST_BASE_ADDRESS;
    res    = &(pdev->dev->resource[idx]);

    if ( acc == ACC_WRITE )
    {
        if ( (*val == 0xffffffff) || 
             ((res->flags & IORESOURCE_IO) && (*val == 0xffff)) )
        {
            /* Set bit and return. */
            set_bit(st_bit, &pdev->state);
            ret = 0;
        }
        else
        {
            /* Assume guest wants to set the base address. */
            clear_bit(st_bit, &pdev->state);

            /* check if guest tries to restore orig value */
            ret = pci_read_config_dword(dev, reg, &orig_val);
            if ( (ret == 0) && (*val != orig_val) ) 
            {
                INFO("Guest attempting update to BASE_ADDRESS %d\n", idx);
                ret = -EPERM;
            }
        }
        VERBOSE_INFO("fixed pci write: %02x:%02x:%02x reg=0x%02x len=0x%02x"
                     " val=0x%08x %x\n", 
                     dev->bus->number, PCI_SLOT(dev->devfn), 
                     PCI_FUNC(dev->devfn), reg, len, *val, pdev->state);
    }
    else if ( acc == ACC_READ )
    {
        ret = pci_read_config_dword(dev, reg, val);
        if ( (ret == 0) && test_bit(st_bit, &pdev->state) )
        {
            /* Cook the value. */
            sz  = res->end - res->start;
            if ( res->flags & IORESOURCE_MEM )
            {
                /* this is written out explicitly for clarity */
                *val = 0xffffffff;
                /* bit    0 = 0 */
                /* bit  21  = memory type */
                /* bit 3    = prefetchable */
                /* bit 4-31 width */
                sz   = sz >> 4; /* size in blocks of 16 byte */
                sz   = ~sz;     /* invert */
                *val = *val & (sz << 4); /* and in the size */
                /* use read values for low 4 bits */
                *val = *val | (orig_val & 0xf);
            }
            else if ( res->flags & IORESOURCE_IO )
            {
                *val = 0x0000ffff;
                /* bit 10 = 01 */
                /* bit 2-31 width */
                sz   = sz >> 2; /* size in dwords */
                sz   = ~sz & 0x0000ffff;
                *val = *val & (sz << 2);
                *val = *val | 0x1;
            }
        }
        VERBOSE_INFO("fixed pci read: %02x:%02x:%02x reg=0x%02x len=0x%02x"
                     " val=0x%08x %x\n", 
                     dev->bus->number, PCI_SLOT(dev->devfn), 
                     PCI_FUNC(dev->devfn), reg, len, *val, pdev->state);
    }

    return ret;
}


static int do_rom_address_access(phys_dev_t *pdev, int acc, int len, u32 *val)
{
    int st_bit, ret = -EINVAL;
    struct pci_dev *dev = pdev->dev;
    u32 orig_val, sz;
    struct resource *res;

    if ( len != sizeof(u32) )
    {
        INFO("Guest attempting sub-dword %s to ROM_ADDRESS\n", 
             (acc == ACC_READ) ? "read" : "write");
        return -EPERM;
    }

    st_bit = ST_ROM_ADDRESS;
    res = &(pdev->dev->resource[PCI_ROM_RESOURCE]);

    if ( acc == ACC_WRITE )
    {
        if ( (*val == 0xffffffff) || (*val == 0xfffffffe) )
        {
            /* NB. 0xffffffff would be unusual, but we trap it anyway. */
            set_bit(st_bit, &pdev->state);
            ret = 0;
        }
        else
        {
            /* Assume guest wants simply to set the base address. */
            clear_bit(st_bit, &pdev->state);
            
            /* Check if guest tries to restore the original value. */
            ret = pci_read_config_dword(dev, PCI_ROM_ADDRESS, &orig_val);
            if ( (ret == 0) && (*val != orig_val) ) 
            {
                if ( (*val != 0x00000000) )
                {
                    INFO("caution: guest tried to change rom address.\n");
                    ret = -EPERM;
                }
                else
                {
                    INFO("guest disabled rom access for %02x:%02x:%02x\n",
                         dev->bus->number, PCI_SLOT(dev->devfn), 
                         PCI_FUNC(dev->devfn));
                }
            }
        }
        VERBOSE_INFO("fixed pci write: %02x:%02x:%02x reg=0x%02x len=0x%02x"
                     " val=0x%08x %x\n", 
                     dev->bus->number, PCI_SLOT(dev->devfn), 
                     PCI_FUNC(dev->devfn), PCI_ROM_ADDRESS, len, *val, pdev->state);
    }
    else if ( acc == ACC_READ )
    {
        ret = pci_read_config_dword(dev, PCI_ROM_ADDRESS, val);
        if ( (ret == 0) && test_bit(st_bit, &pdev->state) )
        {
            /* Cook the value. */
            sz  = res->end - res->start;
            *val = 0xffffffff;
            /* leave bit 0 untouched */
            /* bit 1-10 reserved, harwired to 0 */
            sz = sz >> 11; /* size is in 2KB blocks */
            sz = ~sz;
            *val = *val & (sz << 11);
            *val = *val | (orig_val & 0x1);
        }
        VERBOSE_INFO("fixed pci read: %02x:%02x:%02x reg=0x%02x len=0x%02x"
                     " val=0x%08x %x\n", 
                     dev->bus->number, PCI_SLOT(dev->devfn), 
                     PCI_FUNC(dev->devfn), PCI_ROM_ADDRESS, len, *val, pdev->state);
    }

    return ret;

}

/*
 * Handle a PCI config space read access if the domain has access privileges.
 */
static long pci_cfgreg_read(int bus, int dev, int func, int reg,
                            int len, u32 *val)
{
    int ret;
    phys_dev_t *pdev;

    if ( (ret = check_dev_acc(current, bus, dev, func, &pdev)) != 0 )
    {
        /* PCI spec states that reads from non-existent devices should return
         * all 1s.  In this case the domain has no read access, which should
         * also look like the device is non-existent. */
        *val = 0xFFFFFFFF;
        return ret; /* KAF: error return seems to matter on my test machine. */
    }

    /* Fake out read requests for some registers. */
    switch ( reg )
    {
    case PCI_BASE_ADDRESS_0:
        ret = do_base_address_access(pdev, ACC_READ, 0, len, val);
        break;

    case PCI_BASE_ADDRESS_1:
        ret = do_base_address_access(pdev, ACC_READ, 1, len, val);
        break;

    case PCI_BASE_ADDRESS_2:
        ret = do_base_address_access(pdev, ACC_READ, 2, len, val);
        break;

    case PCI_BASE_ADDRESS_3:
        ret = do_base_address_access(pdev, ACC_READ, 3, len, val);
        break;

    case PCI_BASE_ADDRESS_4:
        ret = do_base_address_access(pdev, ACC_READ, 4, len, val);
        break;

    case PCI_BASE_ADDRESS_5:
        ret = do_base_address_access(pdev, ACC_READ, 5, len, val);
        break;

    case PCI_ROM_ADDRESS:
        ret = do_rom_address_access(pdev, ACC_READ, len, val);
        break;        

    case PCI_INTERRUPT_LINE:
        *val = pdev->dev->irq;
        ret = 0;
        break;

    default:
        ret = pci_config_read(0, bus, dev, func, reg, len, val);        
        VERBOSE_INFO("pci read : %02x:%02x:%02x reg=0x%02x len=0x%02x "
                     "val=0x%08x\n", bus, dev, func, reg, len, *val);
        break;
    }

    return ret;
}


/*
 * Handle a PCI config space write access if the domain has access privileges.
 */
static long pci_cfgreg_write(int bus, int dev, int func, int reg,
                             int len, u32 val)
{
    int ret;
    phys_dev_t *pdev;

    if ( (ret = check_dev_acc(current, bus, dev, func, &pdev)) != 0 )
        return ret;

    /* special treatment for some registers */
    switch (reg)
    {
    case PCI_BASE_ADDRESS_0:
        ret = do_base_address_access(pdev, ACC_WRITE, 0, len, &val);
        break;

    case PCI_BASE_ADDRESS_1:
        ret = do_base_address_access(pdev, ACC_WRITE, 1, len, &val);
        break;

    case PCI_BASE_ADDRESS_2:
        ret = do_base_address_access(pdev, ACC_WRITE, 2, len, &val);
        break;

    case PCI_BASE_ADDRESS_3:
        ret = do_base_address_access(pdev, ACC_WRITE, 3, len, &val);
        break;

    case PCI_BASE_ADDRESS_4:
        ret = do_base_address_access(pdev, ACC_WRITE, 4, len, &val);
        break;

    case PCI_BASE_ADDRESS_5:
        ret = do_base_address_access(pdev, ACC_WRITE, 5, len, &val);
        break;

    case PCI_ROM_ADDRESS:
        ret = do_rom_address_access(pdev, ACC_WRITE, len, &val);
        break;        

    default:
        if ( pdev->flags != ACC_WRITE ) 
        {
            INFO("pci write not allowed %02x:%02x:%02x: "
                 "reg=0x%02x len=0x%02x val=0x%08x\n",
                 bus, dev, func, reg, len, val);
            ret = -EPERM;
        }
        else
        {
            ret = pci_config_write(0, bus, dev, func, reg, len, val);
            VERBOSE_INFO("pci write: %02x:%02x:%02x reg=0x%02x len=0x%02x "
                         "val=0x%08x\n", bus, dev, func, reg, len, val);
        }
        break;
    }

    return ret;
}


static long pci_probe_root_buses(u32 *busmask)
{
    phys_dev_t *pdev;
    struct list_head *tmp;

    memset(busmask, 0, 256/8);

    list_for_each ( tmp, &current->pcidev_list )
    {
        pdev = list_entry(tmp, phys_dev_t, node);
        set_bit(pdev->dev->bus->number, busmask);
    }

    return 0;
}


/*
 * Demuxing hypercall.
 */
long do_physdev_op(physdev_op_t *uop)
{
    phys_dev_t  *pdev;
    physdev_op_t op;
    long         ret;
    int          irq;

    if ( unlikely(copy_from_user(&op, uop, sizeof(op)) != 0) )
        return -EFAULT;

    switch ( op.cmd )
    {
    case PHYSDEVOP_PCI_CFGREG_READ:
        ret = pci_cfgreg_read(op.u.pci_cfgreg_read.bus,
                              op.u.pci_cfgreg_read.dev, 
                              op.u.pci_cfgreg_read.func,
                              op.u.pci_cfgreg_read.reg, 
                              op.u.pci_cfgreg_read.len,
                              &op.u.pci_cfgreg_read.value);
        break;

    case PHYSDEVOP_PCI_CFGREG_WRITE:
        ret = pci_cfgreg_write(op.u.pci_cfgreg_write.bus,
                               op.u.pci_cfgreg_write.dev, 
                               op.u.pci_cfgreg_write.func,
                               op.u.pci_cfgreg_write.reg, 
                               op.u.pci_cfgreg_write.len,
                               op.u.pci_cfgreg_write.value);
        break;

    case PHYSDEVOP_PCI_INITIALISE_DEVICE:
        if ( (ret = check_dev_acc(current, 
                                  op.u.pci_initialise_device.bus, 
                                  op.u.pci_initialise_device.dev, 
                                  op.u.pci_initialise_device.func, 
                                  &pdev)) == 0 )
            pcibios_enable_irq(pdev->dev);
        break;

    case PHYSDEVOP_PCI_PROBE_ROOT_BUSES:
        ret = pci_probe_root_buses(op.u.pci_probe_root_buses.busmask);
        break;

    case PHYSDEVOP_IRQ_UNMASK_NOTIFY:
        ret = pirq_guest_unmask(current);
        break;

    case PHYSDEVOP_IRQ_STATUS_QUERY:
        irq = op.u.irq_status_query.irq;
        ret = -EINVAL;
        if ( (irq < 0) || (irq >= NR_IRQS) )
            break;
        op.u.irq_status_query.flags = 0;
        /* Edge-triggered interrupts don't need an explicit unmask downcall. */
        if ( strstr(irq_desc[irq].handler->typename, "edge") == NULL )
            op.u.irq_status_query.flags |= PHYSDEVOP_IRQ_NEEDS_UNMASK_NOTIFY;
        ret = 0;
        break;

    default:
        ret = -EINVAL;
        break;
    }

    copy_to_user(uop, &op, sizeof(op));
    return ret;
}

/* Test if boot params specify this device should NOT be visible to DOM0
 * (e.g. so that another domain can control it instead) */
int pcidev_dom0_hidden(struct pci_dev *dev)
{
    extern char opt_physdev_dom0_hide[];
    char cmp[10] = "(.......)";
    
    strncpy(&cmp[1], dev->slot_name, 7);

    if ( strstr(opt_physdev_dom0_hide, dev->slot_name) == NULL )
        return 0;
    
    return 1;
}


/* Domain 0 has read access to all devices. */
void physdev_init_dom0(struct task_struct *p)
{
    struct pci_dev *dev;
    phys_dev_t *pdev;

    INFO("Give DOM0 read access to all PCI devices\n");

    pci_for_each_dev(dev)
    {
        if ( !pcidev_dom0_hidden(dev) )
        {            
            /* Skip bridges and other peculiarities for now. */
            if ( dev->hdr_type != PCI_HEADER_TYPE_NORMAL )
                continue;
            pdev = kmalloc(sizeof(phys_dev_t), GFP_KERNEL);
            pdev->dev = dev;
            pdev->flags = ACC_WRITE;
            pdev->state = 0;
            pdev->owner = p;
            list_add(&pdev->node, &p->pcidev_list);
        }
        else
        {
            printk("Hiding PCI device %s from DOM0\n", dev->slot_name);
        }
    }

    set_bit(PF_PHYSDEV, &p->flags);
}

