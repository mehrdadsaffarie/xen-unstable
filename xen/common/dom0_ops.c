/******************************************************************************
 * dom0_ops.c
 * 
 * Process command requests from domain-0 guest OS.
 * 
 * Copyright (c) 2002, K A Fraser
 */

#include <xeno/config.h>
#include <xeno/types.h>
#include <xeno/lib.h>
#include <xeno/mm.h>
#include <xeno/dom0_ops.h>
#include <xeno/sched.h>
#include <xeno/event.h>
#include <asm/domain_page.h>

extern unsigned int alloc_new_dom_mem(struct task_struct *, unsigned int);

static unsigned int get_domnr(void)
{
    static unsigned int domnr = 0;
    do { domnr = (domnr+1) & ((1<<20)-1); }
    while ( find_domain_by_id(domnr) != NULL );
    return domnr;
}

static void build_page_list(struct task_struct *p)
{
    unsigned long *list;
    unsigned long curr;
    struct list_head *list_ent;

    curr = list_entry(p->pg_head.next, struct pfn_info, list) - frame_table;
    list = (unsigned long *)map_domain_mem(curr << PAGE_SHIFT);

    list_for_each(list_ent, &p->pg_head)
    {
        *list++ = list_entry(list_ent, struct pfn_info, list) - frame_table;

        if( ((unsigned long)list & ~PAGE_MASK) == 0 )
        {
            struct list_head *ent = frame_table[curr].list.next;
            curr = list_entry(ent, struct pfn_info, list) - frame_table;
            unmap_domain_mem(list-1);
            list = (unsigned long *)map_domain_mem(curr << PAGE_SHIFT);
        }
    }

    unmap_domain_mem(list);
}
    
long do_dom0_op(dom0_op_t *u_dom0_op)
{
    long ret = 0;
    dom0_op_t op;

    if ( current->domain != 0 )
        return -EPERM;

    if ( copy_from_user(&op, u_dom0_op, sizeof(op)) )
        return -EFAULT;

    switch ( op.cmd )
    {

    case DOM0_BUILDDOMAIN:
    {
        struct task_struct * p = find_domain_by_id(op.u.meminfo.domain);
        if ( (ret = final_setup_guestos(p, &op.u.meminfo)) != 0 )
            break;
        ret = p->domain;
        free_task_struct(p);
    }
    break;

    case DOM0_STARTDOMAIN:
    {
        struct task_struct * p = find_domain_by_id(op.u.meminfo.domain);
        ret = -EINVAL;
        if ( (p == NULL) || !(p->flags & PF_CONSTRUCTED) )
            break;
        wake_up(p);
        reschedule(p);
        ret = p->domain;
        free_task_struct(p);
    }
    break;

    case DOM0_STOPDOMAIN:
    {
      ret = stop_other_domain (op.u.meminfo.domain);
    }
    break;

    case DOM0_CREATEDOMAIN:
    {
        struct task_struct *p;
        static unsigned int pro = 0;
        unsigned int dom = get_domnr();
        ret = -ENOMEM;

        if ( dom == 0 ) 
            break;

        pro = (pro+1) % smp_num_cpus;
        p = do_newdomain(dom, pro);
        if ( p == NULL ) 
            break;

	if (op.u.newdomain.name[0]) {
	  strncpy (p -> name, op.u.newdomain.name, MAX_DOMAIN_NAME);
	  p -> name[MAX_DOMAIN_NAME - 1] = 0;
	}

        ret = alloc_new_dom_mem(p, op.u.newdomain.memory_kb);
        if ( ret != 0 ) 
        {
            __kill_domain(p);
            break;
        }

        build_page_list(p);
        
        ret = p->domain;
        
        op.u.newdomain.domain = ret;
        op.u.newdomain.pg_head = 
            list_entry(p->pg_head.next, struct pfn_info, list) -
            frame_table;
        copy_to_user(u_dom0_op, &op, sizeof(op));
    }
    break;

    case DOM0_DESTROYDOMAIN:
    {
        unsigned int dom = op.u.killdomain.domain;
        int force = op.u.killdomain.force;
        if ( dom == IDLE_DOMAIN_ID )
        {
            ret = -EPERM;
        }
        else
        {
            ret = kill_other_domain(dom, force);
        }
    }
    break;

    case DOM0_BVTCTL:
    {
        unsigned long  ctx_allow = op.u.bvtctl.ctx_allow;
        ret = sched_bvtctl(ctx_allow);
        
    }
    break;

    case DOM0_ADJUSTDOM:
    {
        unsigned int   dom     = op.u.adjustdom.domain;
        unsigned long  mcu_adv = op.u.adjustdom.mcu_adv;
        unsigned long  warp    = op.u.adjustdom.warp;
        unsigned long  warpl   = op.u.adjustdom.warpl;
        unsigned long  warpu   = op.u.adjustdom.warpu;
        

        if ( dom == IDLE_DOMAIN_ID )
        {
            ret = -EPERM;
        }
        else
        {
            ret = sched_adjdom(dom, mcu_adv, warp, warpl, warpu);
        }
    }
    break;

    case DOM0_GETMEMLIST:
    {
        int i;
        unsigned long pfn = op.u.getmemlist.start_pfn;
        unsigned long *buffer = op.u.getmemlist.buffer;
        struct list_head *list_ent;

        for ( i = 0; i < op.u.getmemlist.num_pfns; i++ )
        {
            /* XXX We trust DOM0 to give us a safe buffer. XXX */
            *buffer++ = pfn;
            list_ent = frame_table[pfn].list.next;
            pfn = list_entry(list_ent, struct pfn_info, list) - frame_table;
        }
    }
    break;

    case DOM0_GETDOMAININFO:
    { 
      struct task_struct *p;
      u_long flags;

      p = idle0_task.next_task;
      read_lock_irqsave (&tasklist_lock, flags);
      do {
        if ((!is_idle_task (p)) && (p -> domain >= op.u.getdominfo.domain)) {
          break;
        }
      } while ((p = p -> next_task) != &idle0_task);

      if (p == &idle0_task) {
        ret = -ESRCH;
      } else {
        op.u.getdominfo.domain = p -> domain;
        strcpy (op.u.getdominfo.name, p -> name);
        op.u.getdominfo.processor = p -> processor;
        op.u.getdominfo.has_cpu = p -> has_cpu;
        op.u.getdominfo.state = p -> state;
        op.u.getdominfo.hyp_events = p -> hyp_events;
        op.u.getdominfo.mcu_advance = p -> mcu_advance;
        op.u.getdominfo.pg_head = list_entry(p->pg_head.next,
                                             struct pfn_info, list) - frame_table;
        op.u.getdominfo.tot_pages = p -> tot_pages;
      }
      read_unlock_irqrestore (&tasklist_lock, flags);
      copy_to_user(u_dom0_op, &op, sizeof(op));
      break;
    }

    default:
        ret = -ENOSYS;

    }

    return ret;
}
