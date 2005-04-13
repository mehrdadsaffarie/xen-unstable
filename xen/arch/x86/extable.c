
#include <xen/config.h>
#include <xen/spinlock.h>
#include <asm/uaccess.h>

#ifdef PERF_COUNTERS
#include <xen/sched.h>
#include <xen/perfc.h>
#endif

extern struct exception_table_entry __start___ex_table[];
extern struct exception_table_entry __stop___ex_table[];
extern struct exception_table_entry __start___pre_ex_table[];
extern struct exception_table_entry __stop___pre_ex_table[];

static void sort_exception_table(struct exception_table_entry *start,
                                 struct exception_table_entry *end)
{
    struct exception_table_entry *p, *q, tmp;

    for ( p = start; p < end; p++ )
    {
        for ( q = p-1; q > start; q-- )
            if ( p->insn > q->insn )
                break;
        if ( ++q != p )
        {
            tmp = *p;
            memmove(q+1, q, (p-q)*sizeof(*p));
            *q = tmp;
        }
    }
}

void sort_exception_tables(void)
{
    sort_exception_table(__start___ex_table, __stop___ex_table);
    sort_exception_table(__start___pre_ex_table, __stop___pre_ex_table);
}

static inline unsigned long
search_one_table(const struct exception_table_entry *first,
		 const struct exception_table_entry *last,
		 unsigned long value)
{
    const struct exception_table_entry *mid;
    long diff;

    while ( first <= last )
    {
        mid = (last - first) / 2 + first;
        diff = mid->insn - value;
        if (diff == 0)
            return mid->fixup;
        else if (diff < 0)
            first = mid+1;
        else
            last = mid-1;
    }
    return 0;
}

unsigned long
search_exception_table(unsigned long addr)
{
    return search_one_table(
        __start___ex_table, __stop___ex_table-1, addr);
}

unsigned long
search_pre_exception_table(struct xen_regs *regs)
{
    unsigned long addr = (unsigned long)regs->eip;
    unsigned long fixup = search_one_table(
        __start___pre_ex_table, __stop___pre_ex_table-1, addr);
    DPRINTK("Pre-exception: %p -> %p\n", addr, fixup);
#ifdef PERF_COUNTERS
    if ( fixup )
        perfc_incrc(exception_fixed);
#endif
    return fixup;
}
