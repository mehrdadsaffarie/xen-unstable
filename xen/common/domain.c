
#include <xen/config.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/errno.h>
#include <xen/sched.h>
#include <xen/mm.h>
#include <xen/event.h>
#include <xen/time.h>
#include <xen/console.h>
#include <xen/shadow.h>
#include <xen/elf.h>
#include <hypervisor-ifs/dom0_ops.h>
#include <asm/hardirq.h>
#include <asm/domain_page.h>

/* Both these structures are protected by the tasklist_lock. */
rwlock_t tasklist_lock __cacheline_aligned = RW_LOCK_UNLOCKED;
struct domain *task_hash[TASK_HASH_SIZE];
struct domain *task_list;

extern void arch_do_createdomain(struct domain *);
extern void arch_final_setup_guestos(struct domain *, full_execution_context_t *c);
extern void free_perdomain_pt(struct domain *);
extern void domain_relinquish_memory(struct domain *d);

struct domain *do_createdomain(domid_t dom_id, unsigned int cpu)
{
    char buf[100];
    struct domain *d, **pd;
    unsigned long flags;

    if ( (d = alloc_domain_struct()) == NULL )
        return NULL;

    atomic_set(&d->refcnt, 1);
    atomic_set(&d->pausecnt, 0);

    shadow_lock_init(d);

    d->domain    = dom_id;
    d->processor = cpu;
    d->create_time = NOW();

    memcpy(&d->thread, &idle0_task.thread, sizeof(d->thread));

    if ( d->domain != IDLE_DOMAIN_ID )
    {
        if ( init_event_channels(d) != 0 )
        {
            free_domain_struct(d);
            return NULL;
        }
        
        /* We use a large intermediate to avoid overflow in sprintf. */
        sprintf(buf, "Domain-%u", dom_id);
        strncpy(d->name, buf, MAX_DOMAIN_NAME);
        d->name[MAX_DOMAIN_NAME-1] = '\0';

        d->addr_limit = USER_DS;
        
        spin_lock_init(&d->page_alloc_lock);
        INIT_LIST_HEAD(&d->page_list);
        d->max_pages = d->tot_pages = 0;

	arch_do_createdomain(d);

        /* Per-domain PCI-device list. */
        spin_lock_init(&d->pcidev_lock);
        INIT_LIST_HEAD(&d->pcidev_list);

        sched_add_domain(d);

        write_lock_irqsave(&tasklist_lock, flags);
        pd = &task_list; /* NB. task_list is maintained in order of dom_id. */
        for ( pd = &task_list; *pd != NULL; pd = &(*pd)->next_list )
            if ( (*pd)->domain > d->domain )
                break;
        d->next_list = *pd;
        *pd = d;
        d->next_hash = task_hash[TASK_HASH(dom_id)];
        task_hash[TASK_HASH(dom_id)] = d;
        write_unlock_irqrestore(&tasklist_lock, flags);
    }
    else
    {
        sprintf(d->name, "Idle-%d", cpu);
        sched_add_domain(d);
    }

    return d;
}


struct domain *find_domain_by_id(domid_t dom)
{
    struct domain *d;
    unsigned long flags;

    read_lock_irqsave(&tasklist_lock, flags);
    d = task_hash[TASK_HASH(dom)];
    while ( d != NULL )
    {
        if ( d->domain == dom )
        {
            if ( unlikely(!get_domain(d)) )
                d = NULL;
            break;
        }
        d = d->next_hash;
    }
    read_unlock_irqrestore(&tasklist_lock, flags);

    return d;
}


/* Return the most recently created domain. */
struct domain *find_last_domain(void)
{
    struct domain *d, *dlast;
    unsigned long flags;

    read_lock_irqsave(&tasklist_lock, flags);
    dlast = task_list;
    d = dlast->next_list;
    while ( d != NULL )
    {
        if ( d->create_time > dlast->create_time )
            dlast = d;
        d = d->next_list;
    }
    if ( !get_domain(dlast) )
        dlast = NULL;
    read_unlock_irqrestore(&tasklist_lock, flags);

    return dlast;
}


void domain_kill(struct domain *d)
{
    domain_pause(d);
    if ( !test_and_set_bit(DF_DYING, &d->flags) )
    {
        sched_rem_domain(d);
        domain_relinquish_memory(d);
        put_domain(d);
    }
}


void domain_crash(void)
{
    struct domain *d;

    set_bit(DF_CRASHED, &current->flags);

    d = find_domain_by_id(0);
    send_guest_virq(d, VIRQ_DOM_EXC);
    put_domain(d);
    
    __enter_scheduler();
    BUG();
}

void domain_shutdown(u8 reason)
{
    struct domain *d;

    if ( current->domain == 0 )
    {
        extern void machine_restart(char *);
        printk("Domain 0 shutdown: rebooting machine!\n");
        machine_restart(0);
    }

    current->shutdown_code = reason;
    set_bit(DF_SHUTDOWN, &current->flags);

    d = find_domain_by_id(0);
    send_guest_virq(d, VIRQ_DOM_EXC);
    put_domain(d);

    __enter_scheduler();
}

struct pfn_info *alloc_domain_page(struct domain *d)
{
    struct pfn_info *page = NULL;
    unsigned long flags, mask, pfn_stamp, cpu_stamp;
    int i;

    ASSERT(!in_irq());

    spin_lock_irqsave(&free_list_lock, flags);
    if ( likely(!list_empty(&free_list)) )
    {
        page = list_entry(free_list.next, struct pfn_info, list);
        list_del(&page->list);
        free_pfns--;
    }
    spin_unlock_irqrestore(&free_list_lock, flags);

    if ( unlikely(page == NULL) )
        return NULL;

    if ( (mask = page->u.cpu_mask) != 0 )
    {
        pfn_stamp = page->tlbflush_timestamp;
        for ( i = 0; (mask != 0) && (i < smp_num_cpus); i++ )
        {
            if ( mask & (1<<i) )
            {
                cpu_stamp = tlbflush_time[i];
                if ( !NEED_FLUSH(cpu_stamp, pfn_stamp) )
                    mask &= ~(1<<i);
            }
        }

        if ( unlikely(mask != 0) )
        {
            flush_tlb_mask(mask);
            perfc_incrc(need_flush_tlb_flush);
        }
    }

    page->u.domain = d;
    page->type_and_flags = 0;
    if ( d != NULL )
    {
        wmb(); /* Domain pointer must be visible before updating refcnt. */
        spin_lock(&d->page_alloc_lock);
        if ( unlikely(d->tot_pages >= d->max_pages) )
        {
            DPRINTK("Over-allocation for domain %u: %u >= %u\n",
                    d->domain, d->tot_pages, d->max_pages);
            spin_unlock(&d->page_alloc_lock);
            goto free_and_exit;
        }
        list_add_tail(&page->list, &d->page_list);
        page->count_and_flags = PGC_allocated | 1;
        if ( unlikely(d->tot_pages++ == 0) )
            get_domain(d);
        spin_unlock(&d->page_alloc_lock);
    }

    return page;

 free_and_exit:
    spin_lock_irqsave(&free_list_lock, flags);
    list_add(&page->list, &free_list);
    free_pfns++;
    spin_unlock_irqrestore(&free_list_lock, flags);
    return NULL;
}

void free_domain_page(struct pfn_info *page)
{
    unsigned long  flags;
    int            drop_dom_ref;
    struct domain *d = page->u.domain;

    if ( unlikely(IS_XEN_HEAP_FRAME(page)) )
    {
        spin_lock_recursive(&d->page_alloc_lock);
        drop_dom_ref = (--d->xenheap_pages == 0);
        spin_unlock_recursive(&d->page_alloc_lock);
    }
    else
    {
        page->tlbflush_timestamp = tlbflush_clock;
        page->u.cpu_mask = 1 << d->processor;
        
        /* NB. May recursively lock from domain_relinquish_memory(). */
        spin_lock_recursive(&d->page_alloc_lock);
        list_del(&page->list);
        drop_dom_ref = (--d->tot_pages == 0);
        spin_unlock_recursive(&d->page_alloc_lock);

        page->count_and_flags = 0;
        
        spin_lock_irqsave(&free_list_lock, flags);
        list_add(&page->list, &free_list);
        free_pfns++;
        spin_unlock_irqrestore(&free_list_lock, flags);
    }

    if ( drop_dom_ref )
        put_domain(d);
}

unsigned int alloc_new_dom_mem(struct domain *d, unsigned int kbytes)
{
    unsigned int alloc_pfns, nr_pages;
    struct pfn_info *page;

    nr_pages = (kbytes + ((PAGE_SIZE-1)>>10)) >> (PAGE_SHIFT - 10);
    d->max_pages = nr_pages; /* this can now be controlled independently */

    /* Grow the allocation if necessary. */
    for ( alloc_pfns = d->tot_pages; alloc_pfns < nr_pages; alloc_pfns++ )
    {
        if ( unlikely((page=alloc_domain_page(d)) == NULL) ||
             unlikely(free_pfns < (SLACK_DOMAIN_MEM_KILOBYTES >> 
                                   (PAGE_SHIFT-10))) )
        {
            domain_relinquish_memory(d);
            return -ENOMEM;
        }

        /* initialise to machine_to_phys_mapping table to likely pfn */
        machine_to_phys_mapping[page-frame_table] = alloc_pfns;

#ifndef NDEBUG
        {
            /* Initialise with magic marker if in DEBUG mode. */
            void *a = map_domain_mem((page-frame_table)<<PAGE_SHIFT);
            memset(a, 0x80 | (char)d->domain, PAGE_SIZE);
            unmap_domain_mem(a);
        }
#endif
    }

    return 0;
}
 

/* Release resources belonging to task @p. */
void domain_destruct(struct domain *d)
{
    struct domain **pd;
    unsigned long flags;

    if ( !test_bit(DF_DYING, &d->flags) )
        BUG();

    /* May be already destructed, or get_domain() can race us. */
    if ( cmpxchg(&d->refcnt.counter, 0, DOMAIN_DESTRUCTED) != 0 )
        return;

    DPRINTK("Releasing task %u\n", d->domain);

    /* Delete from task list and task hashtable. */
    write_lock_irqsave(&tasklist_lock, flags);
    pd = &task_list;
    while ( *pd != d ) 
        pd = &(*pd)->next_list;
    *pd = d->next_list;
    pd = &task_hash[TASK_HASH(d->domain)];
    while ( *pd != d ) 
        pd = &(*pd)->next_hash;
    *pd = d->next_hash;
    write_unlock_irqrestore(&tasklist_lock, flags);

    destroy_event_channels(d);

    free_perdomain_pt(d);
    free_page((unsigned long)d->shared_info);

    free_domain_struct(d);
}


/*
 * final_setup_guestos is used for final setup and launching of domains other
 * than domain 0. ie. the domains that are being built by the userspace dom0
 * domain builder.
 */
int final_setup_guestos(struct domain *p, dom0_builddomain_t *builddomain)
{
    int rc = 0;
    full_execution_context_t *c;

    if ( (c = kmalloc(sizeof(*c))) == NULL )
        return -ENOMEM;

    if ( test_bit(DF_CONSTRUCTED, &p->flags) )
    {
        rc = -EINVAL;
        goto out;
    }

    if ( copy_from_user(c, builddomain->ctxt, sizeof(*c)) )
    {
        rc = -EFAULT;
        goto out;
    }
    
    arch_final_setup_guestos(p,c);

    /* Set up the shared info structure. */
    update_dom_time(p->shared_info);

    set_bit(DF_CONSTRUCTED, &p->flags);

 out:    
    if (c) kfree(c);
    
    return rc;
}

static inline int is_loadable_phdr(Elf_Phdr *phdr)
{
    return ((phdr->p_type == PT_LOAD) &&
            ((phdr->p_flags & (PF_W|PF_X)) != 0));
}

int readelfimage_base_and_size(char *elfbase, 
                                      unsigned long elfsize,
                                      unsigned long *pkernstart,
                                      unsigned long *pkernend,
                                      unsigned long *pkernentry)
{
    Elf_Ehdr *ehdr = (Elf_Ehdr *)elfbase;
    Elf_Phdr *phdr;
    Elf_Shdr *shdr;
    unsigned long kernstart = ~0UL, kernend=0UL;
    char *shstrtab, *guestinfo;
    int h;

    if ( !IS_ELF(*ehdr) )
    {
        printk("Kernel image does not have an ELF header.\n");
        return -EINVAL;
    }

    if ( (ehdr->e_phoff + (ehdr->e_phnum * ehdr->e_phentsize)) > elfsize )
    {
        printk("ELF program headers extend beyond end of image.\n");
        return -EINVAL;
    }

    if ( (ehdr->e_shoff + (ehdr->e_shnum * ehdr->e_shentsize)) > elfsize )
    {
        printk("ELF section headers extend beyond end of image.\n");
        return -EINVAL;
    }

    /* Find the section-header strings table. */
    if ( ehdr->e_shstrndx == SHN_UNDEF )
    {
        printk("ELF image has no section-header strings table (shstrtab).\n");
        return -EINVAL;
    }
    shdr = (Elf_Shdr *)(elfbase + ehdr->e_shoff + 
                        (ehdr->e_shstrndx*ehdr->e_shentsize));
    shstrtab = elfbase + shdr->sh_offset;
    
    /* Find the special '__xen_guest' section and check its contents. */
    for ( h = 0; h < ehdr->e_shnum; h++ )
    {
        shdr = (Elf_Shdr *)(elfbase + ehdr->e_shoff + (h*ehdr->e_shentsize));
        if ( strcmp(&shstrtab[shdr->sh_name], "__xen_guest") != 0 )
            continue;
        guestinfo = elfbase + shdr->sh_offset;
        printk("Xen-ELF header found: '%s'\n", guestinfo);
        if ( (strstr(guestinfo, "GUEST_OS=linux") == NULL) ||
             (strstr(guestinfo, "XEN_VER=1.3") == NULL) )
        {
            printk("ERROR: Xen will only load Linux built for Xen v1.3\n");
            return -EINVAL;
        }
        break;
    }
    if ( h == ehdr->e_shnum )
    {
        printk("Not a Xen-ELF image: '__xen_guest' section not found.\n");
        return -EINVAL;
    }

    for ( h = 0; h < ehdr->e_phnum; h++ ) 
    {
        phdr = (Elf_Phdr *)(elfbase + ehdr->e_phoff + (h*ehdr->e_phentsize));
        if ( !is_loadable_phdr(phdr) )
            continue;
        if ( phdr->p_vaddr < kernstart )
            kernstart = phdr->p_vaddr;
        if ( (phdr->p_vaddr + phdr->p_memsz) > kernend )
            kernend = phdr->p_vaddr + phdr->p_memsz;
    }

    if ( (kernstart > kernend) || 
         (ehdr->e_entry < kernstart) || 
         (ehdr->e_entry > kernend) )
    {
        printk("Malformed ELF image.\n");
        return -EINVAL;
    }

    *pkernstart = kernstart;
    *pkernend   = kernend;
    *pkernentry = ehdr->e_entry;

    return 0;
}

int loadelfimage(char *elfbase)
{
    Elf_Ehdr *ehdr = (Elf_Ehdr *)elfbase;
    Elf_Phdr *phdr;
    int h;
  
    for ( h = 0; h < ehdr->e_phnum; h++ ) 
    {
        phdr = (Elf_Phdr *)(elfbase + ehdr->e_phoff + (h*ehdr->e_phentsize));
        if ( !is_loadable_phdr(phdr) )
            continue;
        if ( phdr->p_filesz != 0 )
            memcpy((char *)phdr->p_vaddr, elfbase + phdr->p_offset, 
                   phdr->p_filesz);
        if ( phdr->p_memsz > phdr->p_filesz )
            memset((char *)phdr->p_vaddr + phdr->p_filesz, 0, 
                   phdr->p_memsz - phdr->p_filesz);
    }

    return 0;
}
