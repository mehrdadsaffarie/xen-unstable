#ifndef _I386_PGTABLE_2LEVEL_H
#define _I386_PGTABLE_2LEVEL_H

/*
 * traditional i386 two-level paging structure:
 */

#define PGDIR_SHIFT	22
#define PTRS_PER_PGD	1024

/*
 * the i386 is two-level, so we don't really have any
 * PMD directory physically.
 */
#define PMD_SHIFT	22
#define PTRS_PER_PMD	1

#define PTRS_PER_PTE	1024

#define pte_ERROR(e) \
	printk("%s:%d: bad pte %08lx.\n", __FILE__, __LINE__, (e).pte_low)
#define pmd_ERROR(e) \
	printk("%s:%d: bad pmd %08lx.\n", __FILE__, __LINE__, pmd_val(e))
#define pgd_ERROR(e) \
	printk("%s:%d: bad pgd %08lx.\n", __FILE__, __LINE__, pgd_val(e))

/*
 * The "pgd_xxx()" functions here are trivial for a folded two-level
 * setup: the pgd is never bad, and a pmd always exists (as it's folded
 * into the pgd entry)
 */
static inline int pgd_none(pgd_t pgd)		{ return 0; }
static inline int pgd_bad(pgd_t pgd)		{ return 0; }
static inline int pgd_present(pgd_t pgd)	{ return 1; }
#define pgd_clear(xp)				do { } while (0)

#define set_pte(pteptr, pteval) queue_l1_entry_update(__pa(pteptr), (pteval).pte_low)
#define set_pte_atomic(pteptr, pteval) queue_l1_entry_update(__pa(pteptr), (pteval).pte_low)
#define set_pmd(pmdptr, pmdval) queue_l2_entry_update(__pa(pmdptr), (pmdval).pmd)
#define set_pgd(pgdptr, pgdval) ((void)0)

#define pgd_page(pgd) \
((unsigned long) __va(pgd_val(pgd) & PAGE_MASK))

static inline pmd_t * pmd_offset(pgd_t * dir, unsigned long address)
{
	return (pmd_t *) dir;
}

/*
 * A note on implementation of this atomic 'get-and-clear' operation.
 * This is actually very simple because XenoLinux can only run on a single
 * processor. Therefore, we cannot race other processors setting the 'accessed'
 * or 'dirty' bits on a page-table entry.
 * Even if pages are shared between domains, that is not a problem because
 * each domain will have separate page tables, with their own versions of
 * accessed & dirty state.
 */
static inline pte_t ptep_get_and_clear(pte_t *xp)
{
    pte_t pte = *xp;
    queue_l1_entry_update(__pa(xp), 0);
    return pte;
}

#define pte_same(a, b)		((a).pte_low == (b).pte_low)
#define pte_page(x)		(mem_map+((unsigned long)((pte_val(x) >> PAGE_SHIFT))))
#define pte_none(x)		(!(x).pte_low)
#define __mk_pte(page_nr,pgprot) __pte(((page_nr) << PAGE_SHIFT) | pgprot_val(pgprot))

#endif /* _I386_PGTABLE_2LEVEL_H */
