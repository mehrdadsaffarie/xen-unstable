/******************************************************************************
 * arch/x86/x86_64/mm.c
 * 
 * Modifications to Linux original are copyright (c) 2004, K A Fraser
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <xen/config.h>
#include <xen/lib.h>
#include <xen/init.h>
#include <xen/mm.h>
#include <xen/sched.h>
#include <asm/asm_defns.h>
#include <asm/page.h>
#include <asm/flushtlb.h>
#include <asm/fixmap.h>
#include <asm/msr.h>

static void *safe_page_alloc(void)
{
    extern int early_boot;
    if ( early_boot )
    {
        unsigned long p = alloc_boot_pages(PAGE_SIZE, PAGE_SIZE);
        if ( p == 0 )
            goto oom;
        return phys_to_virt(p);
    }
    else
    {
        struct pfn_info *pg = alloc_domheap_page(NULL);
        if ( pg == NULL )
            goto oom;
        return page_to_virt(pg);
    }
 oom:
    panic("Out of memory");
    return NULL;
}

/* Map physical byte range (@p, @p+@s) at virt address @v in pagetable @pt. */
#define __PTE_MASK (~(_PAGE_GLOBAL|_PAGE_DIRTY|_PAGE_PCD|_PAGE_PWT))
int map_pages(
    root_pgentry_t *pt,
    unsigned long v,
    unsigned long p,
    unsigned long s,
    unsigned long flags)
{
    l4_pgentry_t *pl4e;
    l3_pgentry_t *pl3e;
    l2_pgentry_t *pl2e;
    l1_pgentry_t *pl1e;
    void         *newpg;

    while ( s != 0 )
    {
        pl4e = &pt[l4_table_offset(v)];
        if ( !(l4e_get_flags(*pl4e) & _PAGE_PRESENT) )
        {
            newpg = safe_page_alloc();
            clear_page(newpg);
            *pl4e = l4e_create_phys(__pa(newpg), flags & __PTE_MASK);
        }

        pl3e = l4e_to_l3e(*pl4e) + l3_table_offset(v);
        if ( !(l3e_get_flags(*pl3e) & _PAGE_PRESENT) )
        {
            newpg = safe_page_alloc();
            clear_page(newpg);
            *pl3e = l3e_create_phys(__pa(newpg), flags & __PTE_MASK);
        }

        pl2e = l3e_to_l2e(*pl3e) + l2_table_offset(v);

        if ( ((s|v|p) & ((1<<L2_PAGETABLE_SHIFT)-1)) == 0 )
        {
            /* Super-page mapping. */
            if ( (l2e_get_flags(*pl2e) & _PAGE_PRESENT) )
                local_flush_tlb_pge();
            *pl2e = l2e_create_phys(p, flags|_PAGE_PSE);

            v += 1 << L2_PAGETABLE_SHIFT;
            p += 1 << L2_PAGETABLE_SHIFT;
            s -= 1 << L2_PAGETABLE_SHIFT;
        }
        else
        {
            /* Normal page mapping. */
            if ( !(l2e_get_flags(*pl2e) & _PAGE_PRESENT) )
            {
                newpg = safe_page_alloc();
                clear_page(newpg);
                *pl2e = l2e_create_phys(__pa(newpg), flags & __PTE_MASK);
            }
            pl1e = l2e_to_l1e(*pl2e) + l1_table_offset(v);
            if ( (l1e_get_flags(*pl1e) & _PAGE_PRESENT) )
                local_flush_tlb_one(v);
            *pl1e = l1e_create_phys(p, flags);

            v += 1 << L1_PAGETABLE_SHIFT;
            p += 1 << L1_PAGETABLE_SHIFT;
            s -= 1 << L1_PAGETABLE_SHIFT;
        }
    }

    return 0;
}

void __set_fixmap(
    enum fixed_addresses idx, unsigned long p, unsigned long flags)
{
    if ( unlikely(idx >= __end_of_fixed_addresses) )
        BUG();
    map_pages(idle_pg_table, fix_to_virt(idx), p, PAGE_SIZE, flags);
}


void __init paging_init(void)
{
    unsigned long i, p, max;
    l3_pgentry_t *l3rw, *l3ro;
    struct pfn_info *pg;

    /* Map all of physical memory. */
    max = ((max_page + L1_PAGETABLE_ENTRIES - 1) & 
           ~(L1_PAGETABLE_ENTRIES - 1)) << PAGE_SHIFT;
    map_pages(idle_pg_table, PAGE_OFFSET, 0, max, PAGE_HYPERVISOR);

    /*
     * Allocate and map the machine-to-phys table.
     * This also ensures L3 is present for ioremap().
     */
    for ( i = 0; i < max_page; i += ((1UL << L2_PAGETABLE_SHIFT) / 8) )
    {
        pg = alloc_domheap_pages(
            NULL, L2_PAGETABLE_SHIFT - L1_PAGETABLE_SHIFT);
        if ( pg == NULL )
            panic("Not enough memory for m2p table\n");
        p = page_to_phys(pg);
        map_pages(idle_pg_table, RDWR_MPT_VIRT_START + i*8, p, 
                  1UL << L2_PAGETABLE_SHIFT, PAGE_HYPERVISOR | _PAGE_USER);
        memset((void *)(RDWR_MPT_VIRT_START + i*8), 0x55,
               1UL << L2_PAGETABLE_SHIFT);
    }

    /*
     * Above we mapped the M2P table as user-accessible and read-writable.
     * Fix security by denying user access at the top level of the page table.
     */
    l4e_remove_flags(&idle_pg_table[l4_table_offset(RDWR_MPT_VIRT_START)],
                     _PAGE_USER);

    /* Create read-only mapping of MPT for guest-OS use. */
    l3ro = (l3_pgentry_t *)alloc_xenheap_page();
    clear_page(l3ro);
    idle_pg_table[l4_table_offset(RO_MPT_VIRT_START)] =
        l4e_create_phys(__pa(l3ro),
                        (__PAGE_HYPERVISOR | _PAGE_USER) & ~_PAGE_RW);

    /* Copy the L3 mappings from the RDWR_MPT area. */
    l3rw = l4e_to_l3e(
        idle_pg_table[l4_table_offset(RDWR_MPT_VIRT_START)]);
    l3rw += l3_table_offset(RDWR_MPT_VIRT_START);
    l3ro += l3_table_offset(RO_MPT_VIRT_START);
    memcpy(l3ro, l3rw,
           (RDWR_MPT_VIRT_END - RDWR_MPT_VIRT_START) >> L3_PAGETABLE_SHIFT);

    /* Set up linear page table mapping. */
    idle_pg_table[l4_table_offset(LINEAR_PT_VIRT_START)] =
        l4e_create_phys(__pa(idle_pg_table), __PAGE_HYPERVISOR);
}

void __init zap_low_mappings(void)
{
    idle_pg_table[0] = l4e_empty();
    flush_tlb_all_pge();
}

void subarch_init_memory(struct domain *dom_xen)
{
    unsigned long i, v, m2p_start_mfn;
    l3_pgentry_t l3e;
    l2_pgentry_t l2e;

    /*
     * We are rather picky about the layout of 'struct pfn_info'. The
     * count_info and domain fields must be adjacent, as we perform atomic
     * 64-bit operations on them.
     */
    if ( (offsetof(struct pfn_info, u.inuse._domain) != 
          (offsetof(struct pfn_info, count_info) + sizeof(u32))) )
    {
        printk("Weird pfn_info layout (%ld,%ld,%ld)\n",
               offsetof(struct pfn_info, count_info),
               offsetof(struct pfn_info, u.inuse._domain),
               sizeof(struct pfn_info));
        for ( ; ; ) ;
    }

    /* M2P table is mappable read-only by privileged domains. */
    for ( v  = RDWR_MPT_VIRT_START; 
          v != RDWR_MPT_VIRT_END;
          v += 1 << L2_PAGETABLE_SHIFT )
    {
        l3e = l4e_to_l3e(idle_pg_table[l4_table_offset(v)])[
            l3_table_offset(v)];
        if ( !(l3e_get_flags(l3e) & _PAGE_PRESENT) )
            continue;
        l2e = l3e_to_l2e(l3e)[l2_table_offset(v)];
        if ( !(l2e_get_flags(l2e) & _PAGE_PRESENT) )
            continue;
        m2p_start_mfn = l2e_get_pfn(l2e);

        for ( i = 0; i < L1_PAGETABLE_ENTRIES; i++ )
        {
            frame_table[m2p_start_mfn+i].count_info = PGC_allocated | 1;
            /* gdt to make sure it's only mapped read-only by non-privileged
               domains. */
            frame_table[m2p_start_mfn+i].u.inuse.type_info = PGT_gdt_page | 1;
            page_set_owner(&frame_table[m2p_start_mfn+i], dom_xen);
        }
    }
}

long do_stack_switch(unsigned long ss, unsigned long esp)
{
    if ( (ss & 3) != 3 )
        return -EPERM;
    current->arch.guest_context.kernel_ss = ss;
    current->arch.guest_context.kernel_sp = esp;
    return 0;
}

long do_set_segment_base(unsigned int which, unsigned long base)
{
    struct exec_domain *ed = current;
    long ret = 0;

    switch ( which )
    {
    case SEGBASE_FS:
        if ( wrmsr_user(MSR_FS_BASE, base, base>>32) )
            ret = -EFAULT;
        else
            ed->arch.guest_context.fs_base = base;
        break;

    case SEGBASE_GS_USER:
        if ( wrmsr_user(MSR_SHADOW_GS_BASE, base, base>>32) )
            ret = -EFAULT;
        else
            ed->arch.guest_context.gs_base_user = base;
        break;

    case SEGBASE_GS_KERNEL:
        if ( wrmsr_user(MSR_GS_BASE, base, base>>32) )
            ret = -EFAULT;
        else
            ed->arch.guest_context.gs_base_kernel = base;
        break;

    case SEGBASE_GS_USER_SEL:
        __asm__ __volatile__ (
            "     swapgs              \n"
            "1:   movl %k0,%%gs       \n"
            "    "safe_swapgs"        \n"
            ".section .fixup,\"ax\"   \n"
            "2:   xorl %k0,%k0        \n"
            "     jmp  1b             \n"
            ".previous                \n"
            ".section __ex_table,\"a\"\n"
            "    .align 8             \n"
            "    .quad 1b,2b          \n"
            ".previous                  "
            : : "r" (base&0xffff) );
        break;

    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}


/* Returns TRUE if given descriptor is valid for GDT or LDT. */
int check_descriptor(struct desc_struct *d)
{
    u32 a = d->a, b = d->b;

    /* A not-present descriptor will always fault, so is safe. */
    if ( !(b & _SEGMENT_P) ) 
        goto good;

    /* The guest can only safely be executed in ring 3. */
    if ( (b & _SEGMENT_DPL) != _SEGMENT_DPL )
        goto bad;

    /* All code and data segments are okay. No base/limit checking. */
    if ( (b & _SEGMENT_S) )
        goto good;

    /* Invalid type 0 is harmless. It is used for 2nd half of a call gate. */
    if ( (b & _SEGMENT_TYPE) == 0x000 )
        goto good;

    /* Everything but a call gate is discarded here. */
    if ( (b & _SEGMENT_TYPE) != 0xc00 )
        goto bad;

    /* Can't allow far jump to a Xen-private segment. */
    if ( !VALID_CODESEL(a>>16) )
        goto bad;

    /* Reserved bits must be zero. */
    if ( (b & 0xe0) != 0 )
        goto bad;
        
 good:
    return 1;
 bad:
    return 0;
}


#ifdef MEMORY_GUARD

#define ALLOC_PT(_level) \
do { \
    (_level) = (_level ## _pgentry_t *)heap_start; \
    heap_start = (void *)((unsigned long)heap_start + PAGE_SIZE); \
    clear_page(_level); \
} while ( 0 )
void *memguard_init(void *heap_start)
{
    l1_pgentry_t *l1 = NULL;
    l2_pgentry_t *l2 = NULL;
    l3_pgentry_t *l3 = NULL;
    l4_pgentry_t *l4 = &idle_pg_table[l4_table_offset(PAGE_OFFSET)];
    unsigned long i, j;

    /* Round the allocation pointer up to a page boundary. */
    heap_start = (void *)(((unsigned long)heap_start + (PAGE_SIZE-1)) & 
                          PAGE_MASK);

    /* Memory guarding is incompatible with super pages. */
    for ( i = 0; i < (xenheap_phys_end >> L2_PAGETABLE_SHIFT); i++ )
    {
        ALLOC_PT(l1);
        for ( j = 0; j < L1_PAGETABLE_ENTRIES; j++ )
            l1[j] = l1e_create_phys((i << L2_PAGETABLE_SHIFT) |
                                    (j << L1_PAGETABLE_SHIFT),
                                    __PAGE_HYPERVISOR);
        if ( !((unsigned long)l2 & (PAGE_SIZE-1)) )
        {
            ALLOC_PT(l2);
            if ( !((unsigned long)l3 & (PAGE_SIZE-1)) )
            {
                ALLOC_PT(l3);
                *l4++ = l4e_create_phys(virt_to_phys(l3), __PAGE_HYPERVISOR);
            }
            *l3++ = l3e_create_phys(virt_to_phys(l2), __PAGE_HYPERVISOR);
        }
        *l2++ = l2e_create_phys(virt_to_phys(l1), __PAGE_HYPERVISOR);
    }

    return heap_start;
}

static void __memguard_change_range(void *p, unsigned long l, int guard)
{
    l1_pgentry_t *l1;
    l2_pgentry_t *l2;
    l3_pgentry_t *l3;
    l4_pgentry_t *l4;
    unsigned long _p = (unsigned long)p;
    unsigned long _l = (unsigned long)l;

    /* Ensure we are dealing with a page-aligned whole number of pages. */
    ASSERT((_p&PAGE_MASK) != 0);
    ASSERT((_l&PAGE_MASK) != 0);
    ASSERT((_p&~PAGE_MASK) == 0);
    ASSERT((_l&~PAGE_MASK) == 0);

    while ( _l != 0 )
    {
        l4 = &idle_pg_table[l4_table_offset(_p)];
        l3 = l4e_to_l3e(*l4) + l3_table_offset(_p);
        l2 = l3e_to_l2e(*l3) + l2_table_offset(_p);
        l1 = l2e_to_l1e(*l2) + l1_table_offset(_p);
        if ( guard )
            l1e_remove_flags(l1, _PAGE_PRESENT);
        else
            l1e_add_flags(l1, _PAGE_PRESENT);
        _p += PAGE_SIZE;
        _l -= PAGE_SIZE;
    }
}

void memguard_guard_stack(void *p)
{
    p = (void *)((unsigned long)p + PAGE_SIZE);
    memguard_guard_range(p, 2 * PAGE_SIZE);
}

void memguard_guard_range(void *p, unsigned long l)
{
    __memguard_change_range(p, l, 1);
    local_flush_tlb();
}

void memguard_unguard_range(void *p, unsigned long l)
{
    __memguard_change_range(p, l, 0);
}

#endif

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
