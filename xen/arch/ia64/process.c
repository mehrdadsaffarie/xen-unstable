/*
 * Miscellaneous process/domain related routines
 * 
 * Copyright (C) 2004 Hewlett-Packard Co.
 *	Dan Magenheimer (dan.magenheimer@hp.com)
 *
 */

#include <xen/config.h>
#include <xen/lib.h>
#include <xen/errno.h>
#include <xen/sched.h>
#include <xen/smp.h>
#include <asm/ptrace.h>
#include <xen/delay.h>

#include <linux/efi.h>	/* FOR EFI_UNIMPLEMENTED */
#include <asm/sal.h>	/* FOR struct ia64_sal_retval */

#include <asm/system.h>
#include <asm/io.h>
#include <asm/processor.h>
#include <asm/desc.h>
//#include <asm/ldt.h>
#include <xen/irq.h>
#include <xen/event.h>
#include <asm/regionreg.h>
#include <asm/privop.h>
#include <asm/vcpu.h>
#include <asm/ia64_int.h>
#include <asm/hpsim_ssc.h>
#include <asm/dom_fw.h>

extern unsigned long vcpu_get_itir_on_fault(struct exec_domain *, UINT64);
extern struct ia64_sal_retval pal_emulator_static(UINT64);
extern struct ia64_sal_retval sal_emulator(UINT64,UINT64,UINT64,UINT64,UINT64,UINT64,UINT64,UINT64);

extern unsigned long dom0_start, dom0_size;

#define IA64_PSR_CPL1	(__IA64_UL(1) << IA64_PSR_CPL1_BIT)
// note IA64_PSR_PK removed from following, why is this necessary?
#define	DELIVER_PSR_SET	(IA64_PSR_IC | IA64_PSR_I | \
			IA64_PSR_DT | IA64_PSR_RT | IA64_PSR_CPL1 | \
			IA64_PSR_IT | IA64_PSR_BN)

#define	DELIVER_PSR_CLR	(IA64_PSR_AC | IA64_PSR_DFL | IA64_PSR_DFH | \
			IA64_PSR_SP | IA64_PSR_DI | IA64_PSR_SI |	\
			IA64_PSR_DB | IA64_PSR_LP | IA64_PSR_TB | \
			IA64_PSR_CPL | IA64_PSR_MC | IA64_PSR_IS | \
			IA64_PSR_ID | IA64_PSR_DA | IA64_PSR_DD | \
			IA64_PSR_SS | IA64_PSR_RI | IA64_PSR_ED | IA64_PSR_IA)

#define PSCB(x,y)	x->vcpu_info->arch.y

extern unsigned long vcpu_verbose;

long do_iopl(domid_t domain, unsigned int new_io_pl)
{
	dummy();
	return 0;
}

void schedule_tail(struct exec_domain *next)
{
	unsigned long rr7;
	printk("current=%lx,shared_info=%lx\n",current,current->vcpu_info);
	printk("next=%lx,shared_info=%lx\n",next,next->vcpu_info);
	if (rr7 = load_region_regs(current)) {
		printk("schedule_tail: change to rr7 not yet implemented\n");
	}
}

extern TR_ENTRY *match_tr(struct exec_domain *ed, unsigned long ifa);

void tdpfoo(void) { }

// given a domain virtual address, pte and pagesize, extract the metaphysical
// address, convert the pte for a physical address for (possibly different)
// Xen PAGE_SIZE and return modified pte.  (NOTE: TLB insert should use
// PAGE_SIZE!)
unsigned long translate_domain_pte(unsigned long pteval,
	unsigned long address, unsigned long itir)
{
	struct domain *d = current->domain;
	unsigned long mask, pteval2, mpaddr;
	unsigned long lookup_domain_mpa(struct domain *,unsigned long);
	extern struct domain *dom0;
	extern unsigned long dom0_start, dom0_size;

	// FIXME address had better be pre-validated on insert
	mask = (1L << ((itir >> 2) & 0x3f)) - 1;
	mpaddr = ((pteval & _PAGE_PPN_MASK) & ~mask) | (address & mask);
	if (d == dom0) {
		if (mpaddr < dom0_start || mpaddr >= dom0_start + dom0_size) {
			//printk("translate_domain_pte: out-of-bounds dom0 mpaddr %p! itc=%lx...\n",mpaddr,ia64_get_itc());
			tdpfoo();
		}
	}
	else if ((mpaddr >> PAGE_SHIFT) > d->max_pages) {
		printf("translate_domain_pte: bad mpa=%p (> %p),vadr=%p,pteval=%p,itir=%p\n",
			mpaddr,d->max_pages<<PAGE_SHIFT,address,pteval,itir);
		tdpfoo();
	}
	pteval2 = lookup_domain_mpa(d,mpaddr);
	pteval2 &= _PAGE_PPN_MASK; // ignore non-addr bits
	pteval2 |= _PAGE_PL_2; // force PL0->2 (PL3 is unaffected)
	pteval2 = (pteval & ~_PAGE_PPN_MASK) | pteval2;
	return pteval2;
}

// given a current domain metaphysical address, return the physical address
unsigned long translate_domain_mpaddr(unsigned long mpaddr)
{
	extern unsigned long lookup_domain_mpa(struct domain *,unsigned long);
	unsigned long pteval;

	if (current->domain == dom0) {
		if (mpaddr < dom0_start || mpaddr >= dom0_start + dom0_size) {
			printk("translate_domain_mpaddr: out-of-bounds dom0 mpaddr %p! continuing...\n",mpaddr);
			tdpfoo();
		}
	}
	pteval = lookup_domain_mpa(current->domain,mpaddr);
	return ((pteval & _PAGE_PPN_MASK) | (mpaddr & ~PAGE_MASK));
}

void reflect_interruption(unsigned long ifa, unsigned long isr, unsigned long itiriim, struct pt_regs *regs, unsigned long vector)
{
	unsigned long vcpu_get_ipsr_int_state(struct exec_domain *,unsigned long);
	unsigned long vcpu_get_rr_ve(struct exec_domain *,unsigned long);
	struct domain *d = current->domain;
	struct exec_domain *ed = current;

	if (vector == IA64_EXTINT_VECTOR) {
		
		extern unsigned long vcpu_verbose, privop_trace;
		static first_extint = 1;
		if (first_extint) {
			printf("Delivering first extint to domain: ifa=%p, isr=%p, itir=%p, iip=%p\n",ifa,isr,itiriim,regs->cr_iip);
			//privop_trace = 1; vcpu_verbose = 1;
			first_extint = 0;
		}
	}
	if (!PSCB(ed,interrupt_collection_enabled)) {
		if (!(PSCB(ed,ipsr) & IA64_PSR_DT)) {
			panic_domain(regs,"psr.dt off, trying to deliver nested dtlb!\n");
		}
		vector &= ~0xf;
		if (vector != IA64_DATA_TLB_VECTOR &&
		    vector != IA64_ALT_DATA_TLB_VECTOR) {
panic_domain(regs,"psr.ic off, delivering fault=%lx,iip=%p,ifa=%p,isr=%p,PSCB.iip=%p\n",
	vector,regs->cr_iip,ifa,isr,PSCB(ed,iip));
			
		}
//printf("Delivering NESTED DATA TLB fault\n");
		vector = IA64_DATA_NESTED_TLB_VECTOR;
		regs->cr_iip = ((unsigned long) PSCB(ed,iva) + vector) & ~0xffUL;
		regs->cr_ipsr = (regs->cr_ipsr & ~DELIVER_PSR_CLR) | DELIVER_PSR_SET;
// NOTE: nested trap must NOT pass PSCB address
		//regs->r31 = (unsigned long) &PSCB(ed);
		return;

	}
	if ((vector & 0xf) != IA64_FORCED_IFA) PSCB(ed,ifa) = ifa;
	else ifa = PSCB(ed,ifa);
	vector &= ~0xf;
//	always deliver on ALT vector (for now?) because no VHPT
//	if (!vcpu_get_rr_ve(ed,ifa)) {
		if (vector == IA64_DATA_TLB_VECTOR)
			vector = IA64_ALT_DATA_TLB_VECTOR;
		else if (vector == IA64_INST_TLB_VECTOR)
			vector = IA64_ALT_INST_TLB_VECTOR;
//	}
	if (vector == IA64_ALT_DATA_TLB_VECTOR ||
	    vector == IA64_ALT_INST_TLB_VECTOR) {
		vcpu_thash(ed,ifa,&PSCB(ed,iha));
	}
	PSCB(ed,unat) = regs->ar_unat;  // not sure if this is really needed?
	PSCB(ed,precover_ifs) = regs->cr_ifs;
	vcpu_bsw0(ed);
	PSCB(ed,ipsr) = vcpu_get_ipsr_int_state(ed,regs->cr_ipsr);
	if (vector == IA64_BREAK_VECTOR || vector == IA64_SPECULATION_VECTOR)
		PSCB(ed,iim) = itiriim;
	else PSCB(ed,itir) = vcpu_get_itir_on_fault(ed,ifa);
	PSCB(ed,isr) = isr; // this is unnecessary except for interrupts!
	PSCB(ed,iip) = regs->cr_iip;
	PSCB(ed,ifs) = 0;
	PSCB(ed,incomplete_regframe) = 0;

	regs->cr_iip = ((unsigned long) PSCB(ed,iva) + vector) & ~0xffUL;
	regs->cr_ipsr = (regs->cr_ipsr & ~DELIVER_PSR_CLR) | DELIVER_PSR_SET;
#ifdef CONFIG_SMP
#error "sharedinfo doesn't handle smp yet"
#endif
	regs->r31 = &((shared_info_t *)SHAREDINFO_ADDR)->vcpu_data[0].arch;

	PSCB(ed,interrupt_delivery_enabled) = 0;
	PSCB(ed,interrupt_collection_enabled) = 0;
}

void foodpi(void) {}

// ONLY gets called from ia64_leave_kernel
// ONLY call with interrupts disabled?? (else might miss one?)
// NEVER successful if already reflecting a trap/fault because psr.i==0
void deliver_pending_interrupt(struct pt_regs *regs)
{
	struct domain *d = current->domain;
	struct exec_domain *ed = current;
	// FIXME: Will this work properly if doing an RFI???
	if (!is_idle_task(d) && user_mode(regs)) {
		//vcpu_poke_timer(ed);
		if (vcpu_deliverable_interrupts(ed)) {
			unsigned long isr = regs->cr_ipsr & IA64_PSR_RI;
			if (vcpu_timer_pending_early(ed))
printf("*#*#*#* about to deliver early timer to domain %d!!!\n",ed->domain->id);
			reflect_interruption(0,isr,0,regs,IA64_EXTINT_VECTOR);
		}
	}
}

int handle_lazy_cover(struct exec_domain *ed, unsigned long isr, struct pt_regs *regs)
{
	if (!PSCB(ed,interrupt_collection_enabled)) {
		if (isr & IA64_ISR_IR) {
//			printf("Handling lazy cover\n");
			PSCB(ed,ifs) = regs->cr_ifs;
			PSCB(ed,incomplete_regframe) = 1;
			regs->cr_ifs = 0;
			return(1); // retry same instruction with cr.ifs off
		}
	}
	return(0);
}

#define IS_XEN_ADDRESS(d,a) ((a >= d->xen_vastart) && (a <= d->xen_vaend))

void xen_handle_domain_access(unsigned long address, unsigned long isr, struct pt_regs *regs, unsigned long itir)
{
	struct domain *d = (struct domain *) current->domain;
	struct domain *ed = (struct exec_domain *) current;
	TR_ENTRY *trp;
	unsigned long psr = regs->cr_ipsr, mask, flags;
	unsigned long iip = regs->cr_iip;
	// FIXME should validate address here
	unsigned long pteval, mpaddr, ps;
	unsigned long lookup_domain_mpa(struct domain *,unsigned long);
	unsigned long match_dtlb(struct exec_domain *,unsigned long, unsigned long *, unsigned long *);
	IA64FAULT fault;

// NEED TO HANDLE THREE CASES:
// 1) domain is in metaphysical mode
// 2) domain address is in TR
// 3) domain address is not in TR (reflect data miss)

		// got here trying to read a privop bundle
	     	//if (d->metaphysical_mode) {
     	if (PSCB(current,metaphysical_mode) && !(address>>61)) {  //FIXME
		if (d == dom0) {
			if (address < dom0_start || address >= dom0_start + dom0_size) {
				printk("xen_handle_domain_access: out-of-bounds"
				   "dom0 mpaddr %p! continuing...\n",mpaddr);
				tdpfoo();
			}
		}
		pteval = lookup_domain_mpa(d,address);
		//FIXME: check return value?
		// would be nice to have a counter here
		vcpu_itc_no_srlz(ed,2,address,pteval,-1UL,PAGE_SHIFT);
		return;
	}
if (address < 0x4000) printf("WARNING: page_fault @%p, iip=%p\n",address,iip);
		
	// if we are fortunate enough to have it in the 1-entry TLB...
	if (pteval = match_dtlb(ed,address,&ps,NULL)) {
		vcpu_itc_no_srlz(ed,6,address,pteval,-1UL,ps);
		return;
	}
	// look in the TRs
	fault = vcpu_tpa(ed,address,&mpaddr);
	if (fault != IA64_NO_FAULT) {
		static int uacnt = 0;
		// can't translate it, just fail (poor man's exception)
		// which results in retrying execution
//printk("*** xen_handle_domain_access: poor man's exception cnt=%i iip=%p, addr=%p...\n",uacnt++,iip,address);
		if (ia64_done_with_exception(regs)) {
//if (!(uacnt++ & 0x3ff)) printk("*** xen_handle_domain_access: successfully handled cnt=%d iip=%p, addr=%p...\n",uacnt,iip,address);
			return;
		}
		else {
			// should never happen.  If it does, region 0 addr may
			// indicate a bad xen pointer
			printk("*** xen_handle_domain_access: exception table"
                               " lookup failed, iip=%p, addr=%p, spinning...\n",
				iip,address);
			panic_domain(regs,"*** xen_handle_domain_access: exception table"
                               " lookup failed, iip=%p, addr=%p, spinning...\n",
				iip,address);
		}
	}
	if (d == dom0) {
		if (mpaddr < dom0_start || mpaddr >= dom0_start + dom0_size) {
			printk("xen_handle_domain_access: vcpu_tpa returned out-of-bounds dom0 mpaddr %p! continuing...\n",mpaddr);
			tdpfoo();
		}
	}
//printk("*** xen_handle_domain_access: tpa resolved miss @%p...\n",address);
	pteval = lookup_domain_mpa(d,mpaddr);
	// would be nice to have a counter here
	//printf("Handling privop data TLB miss\n");
	// FIXME, must be inlined or potential for nested fault here!
	vcpu_itc_no_srlz(ed,2,address,pteval,-1UL,PAGE_SHIFT);
}

void ia64_do_page_fault (unsigned long address, unsigned long isr, struct pt_regs *regs, unsigned long itir)
{
	struct domain *d = (struct domain *) current->domain;
	TR_ENTRY *trp;
	unsigned long psr = regs->cr_ipsr, mask, flags;
	unsigned long iip = regs->cr_iip;
	// FIXME should validate address here
	unsigned long pteval, mpaddr;
	unsigned long lookup_domain_mpa(struct domain *,unsigned long);
	unsigned long is_data = !((isr >> IA64_ISR_X_BIT) & 1UL);
	unsigned long vector;
	IA64FAULT fault;


	//The right way is put in VHPT and take another miss!

	// weak attempt to avoid doing both I/D tlb insert to avoid
	// problems for privop bundle fetch, doesn't work, deal with later
	if (IS_XEN_ADDRESS(d,iip) && !IS_XEN_ADDRESS(d,address)) {
		xen_handle_domain_access(address, isr, regs, itir);

		return;
	}

	// FIXME: no need to pass itir in to this routine as we need to
	// compute the virtual itir anyway (based on domain's RR.ps)
	// AND ACTUALLY reflect_interruption doesn't use it anyway!
	itir = vcpu_get_itir_on_fault(current,address);

	if (PSCB(current,metaphysical_mode) && (is_data || !(address>>61))) {  //FIXME
		// FIXME should validate mpaddr here
		if (d == dom0) {
			if (address < dom0_start || address >= dom0_start + dom0_size) {
				printk("ia64_do_page_fault: out-of-bounds dom0 mpaddr %p, iip=%p! continuing...\n",address,iip);
				printk("ia64_do_page_fault: out-of-bounds dom0 mpaddr %p, old iip=%p!\n",address,current->vcpu_info->arch.iip);
				tdpfoo();
			}
		}
		pteval = lookup_domain_mpa(d,address);
		// FIXME, must be inlined or potential for nested fault here!
		vcpu_itc_no_srlz(current,is_data?2:1,address,pteval,-1UL,PAGE_SHIFT);
		return;
	}
	if (trp = match_tr(current,address)) {
		// FIXME address had better be pre-validated on insert
		pteval = translate_domain_pte(trp->page_flags,address,trp->itir);
		vcpu_itc_no_srlz(current,is_data?2:1,address,pteval,-1UL,(trp->itir>>2)&0x3f);
		return;
	}
	vector = is_data ? IA64_DATA_TLB_VECTOR : IA64_INST_TLB_VECTOR;
	if (handle_lazy_cover(current, isr, regs)) return;
if (!(address>>61)) {
panic_domain(0,"ia64_do_page_fault: @%p???, iip=%p, itc=%p (spinning...)\n",address,iip,ia64_get_itc());
}
	if ((isr & IA64_ISR_SP)
	    || ((isr & IA64_ISR_NA) && (isr & IA64_ISR_CODE_MASK) == IA64_ISR_CODE_LFETCH))
	{
		/*
		 * This fault was due to a speculative load or lfetch.fault, set the "ed"
		 * bit in the psr to ensure forward progress.  (Target register will get a
		 * NaT for ld.s, lfetch will be canceled.)
		 */
		ia64_psr(regs)->ed = 1;
		return;
	}
	reflect_interruption(address, isr, itir, regs, vector);
}

void
ia64_fault (unsigned long vector, unsigned long isr, unsigned long ifa,
	    unsigned long iim, unsigned long itir, unsigned long arg5,
	    unsigned long arg6, unsigned long arg7, unsigned long stack)
{
	struct pt_regs *regs = (struct pt_regs *) &stack;
	unsigned long code, error = isr;
	char buf[128];
	int result, sig;
	static const char *reason[] = {
		"IA-64 Illegal Operation fault",
		"IA-64 Privileged Operation fault",
		"IA-64 Privileged Register fault",
		"IA-64 Reserved Register/Field fault",
		"Disabled Instruction Set Transition fault",
		"Unknown fault 5", "Unknown fault 6", "Unknown fault 7", "Illegal Hazard fault",
		"Unknown fault 9", "Unknown fault 10", "Unknown fault 11", "Unknown fault 12",
		"Unknown fault 13", "Unknown fault 14", "Unknown fault 15"
	};
#if 0
printf("ia64_fault, vector=0x%p, ifa=%p, iip=%p, ipsr=%p, isr=%p\n",
 vector, ifa, regs->cr_iip, regs->cr_ipsr, isr);
#endif

	if ((isr & IA64_ISR_NA) && ((isr & IA64_ISR_CODE_MASK) == IA64_ISR_CODE_LFETCH)) {
		/*
		 * This fault was due to lfetch.fault, set "ed" bit in the psr to cancel
		 * the lfetch.
		 */
		ia64_psr(regs)->ed = 1;
		printf("ia64_fault: handled lfetch.fault\n");
		return;
	}

	switch (vector) {
	      case 24: /* General Exception */
		code = (isr >> 4) & 0xf;
		sprintf(buf, "General Exception: %s%s", reason[code],
			(code == 3) ? ((isr & (1UL << 37))
				       ? " (RSE access)" : " (data access)") : "");
		if (code == 8) {
# ifdef CONFIG_IA64_PRINT_HAZARDS
			printk("%s[%d]: possible hazard @ ip=%016lx (pr = %016lx)\n",
			       current->comm, current->pid, regs->cr_iip + ia64_psr(regs)->ri,
			       regs->pr);
# endif
			printf("ia64_fault: returning on hazard\n");
			return;
		}
		break;

	      case 25: /* Disabled FP-Register */
		if (isr & 2) {
			//disabled_fph_fault(regs);
			//return;
		}
		sprintf(buf, "Disabled FPL fault---not supposed to happen!");
		break;

	      case 26: /* NaT Consumption */
		if (user_mode(regs)) {
			void *addr;

			if (((isr >> 4) & 0xf) == 2) {
				/* NaT page consumption */
				//sig = SIGSEGV;
				//code = SEGV_ACCERR;
				addr = (void *) ifa;
			} else {
				/* register NaT consumption */
				//sig = SIGILL;
				//code = ILL_ILLOPN;
				addr = (void *) (regs->cr_iip + ia64_psr(regs)->ri);
			}
			//siginfo.si_signo = sig;
			//siginfo.si_code = code;
			//siginfo.si_errno = 0;
			//siginfo.si_addr = addr;
			//siginfo.si_imm = vector;
			//siginfo.si_flags = __ISR_VALID;
			//siginfo.si_isr = isr;
			//force_sig_info(sig, &siginfo, current);
			//return;
		} //else if (ia64_done_with_exception(regs))
			//return;
		sprintf(buf, "NaT consumption");
		break;

	      case 31: /* Unsupported Data Reference */
		if (user_mode(regs)) {
			//siginfo.si_signo = SIGILL;
			//siginfo.si_code = ILL_ILLOPN;
			//siginfo.si_errno = 0;
			//siginfo.si_addr = (void *) (regs->cr_iip + ia64_psr(regs)->ri);
			//siginfo.si_imm = vector;
			//siginfo.si_flags = __ISR_VALID;
			//siginfo.si_isr = isr;
			//force_sig_info(SIGILL, &siginfo, current);
			//return;
		}
		sprintf(buf, "Unsupported data reference");
		break;

	      case 29: /* Debug */
	      case 35: /* Taken Branch Trap */
	      case 36: /* Single Step Trap */
		//if (fsys_mode(current, regs)) {}
		switch (vector) {
		      case 29:
			//siginfo.si_code = TRAP_HWBKPT;
#ifdef CONFIG_ITANIUM
			/*
			 * Erratum 10 (IFA may contain incorrect address) now has
			 * "NoFix" status.  There are no plans for fixing this.
			 */
			if (ia64_psr(regs)->is == 0)
			  ifa = regs->cr_iip;
#endif
			break;
		      case 35: ifa = 0; break;
		      case 36: ifa = 0; break;
		      //case 35: siginfo.si_code = TRAP_BRANCH; ifa = 0; break;
		      //case 36: siginfo.si_code = TRAP_TRACE; ifa = 0; break;
		}
		//siginfo.si_signo = SIGTRAP;
		//siginfo.si_errno = 0;
		//siginfo.si_addr  = (void *) ifa;
		//siginfo.si_imm   = 0;
		//siginfo.si_flags = __ISR_VALID;
		//siginfo.si_isr   = isr;
		//force_sig_info(SIGTRAP, &siginfo, current);
		//return;

	      case 32: /* fp fault */
	      case 33: /* fp trap */
		//result = handle_fpu_swa((vector == 32) ? 1 : 0, regs, isr);
		if ((result < 0) || (current->thread.flags & IA64_THREAD_FPEMU_SIGFPE)) {
			//siginfo.si_signo = SIGFPE;
			//siginfo.si_errno = 0;
			//siginfo.si_code = FPE_FLTINV;
			//siginfo.si_addr = (void *) (regs->cr_iip + ia64_psr(regs)->ri);
			//siginfo.si_flags = __ISR_VALID;
			//siginfo.si_isr = isr;
			//siginfo.si_imm = 0;
			//force_sig_info(SIGFPE, &siginfo, current);
		}
		//return;
		sprintf(buf, "FP fault/trap");
		break;

	      case 34:
		if (isr & 0x2) {
			/* Lower-Privilege Transfer Trap */
			/*
			 * Just clear PSR.lp and then return immediately: all the
			 * interesting work (e.g., signal delivery is done in the kernel
			 * exit path).
			 */
			//ia64_psr(regs)->lp = 0;
			//return;
			sprintf(buf, "Lower-Privilege Transfer trap");
		} else {
			/* Unimplemented Instr. Address Trap */
			if (user_mode(regs)) {
				//siginfo.si_signo = SIGILL;
				//siginfo.si_code = ILL_BADIADDR;
				//siginfo.si_errno = 0;
				//siginfo.si_flags = 0;
				//siginfo.si_isr = 0;
				//siginfo.si_imm = 0;
				//siginfo.si_addr = (void *) (regs->cr_iip + ia64_psr(regs)->ri);
				//force_sig_info(SIGILL, &siginfo, current);
				//return;
			}
			sprintf(buf, "Unimplemented Instruction Address fault");
		}
		break;

	      case 45:
		printk(KERN_ERR "Unexpected IA-32 exception (Trap 45)\n");
		printk(KERN_ERR "  iip - 0x%lx, ifa - 0x%lx, isr - 0x%lx\n",
		       regs->cr_iip, ifa, isr);
		//force_sig(SIGSEGV, current);
		break;

	      case 46:
		printk(KERN_ERR "Unexpected IA-32 intercept trap (Trap 46)\n");
		printk(KERN_ERR "  iip - 0x%lx, ifa - 0x%lx, isr - 0x%lx, iim - 0x%lx\n",
		       regs->cr_iip, ifa, isr, iim);
		//force_sig(SIGSEGV, current);
		return;

	      case 47:
		sprintf(buf, "IA-32 Interruption Fault (int 0x%lx)", isr >> 16);
		break;

	      default:
		sprintf(buf, "Fault %lu", vector);
		break;
	}
	//die_if_kernel(buf, regs, error);
printk("ia64_fault: %s: reflecting\n",buf);
reflect_interruption(ifa,isr,iim,regs,IA64_GENEX_VECTOR);
//while(1);
	//force_sig(SIGILL, current);
}

unsigned long running_on_sim = 0;

void
do_ssc(unsigned long ssc, struct pt_regs *regs)
{
	extern unsigned long lookup_domain_mpa(struct domain *,unsigned long);
	unsigned long arg0, arg1, arg2, arg3, retval;
	char buf[2];
/**/	static int last_fd, last_count;	// FIXME FIXME FIXME
/**/					// BROKEN FOR MULTIPLE DOMAINS & SMP
/**/	struct ssc_disk_stat { int fd; unsigned count;} *stat, last_stat;
	extern unsigned long vcpu_verbose, privop_trace;

	arg0 = vcpu_get_gr(current,32);
	switch(ssc) {
	    case SSC_PUTCHAR:
		buf[0] = arg0;
		buf[1] = '\0';
		printf(buf);
		break;
	    case SSC_GETCHAR:
		retval = ia64_ssc(0,0,0,0,ssc);
		vcpu_set_gr(current,8,retval);
		break;
	    case SSC_WAIT_COMPLETION:
		if (arg0) {	// metaphysical address

			arg0 = translate_domain_mpaddr(arg0);
/**/			stat = (struct ssc_disk_stat *)__va(arg0);
///**/			if (stat->fd == last_fd) stat->count = last_count;
/**/			stat->count = last_count;
//if (last_count >= PAGE_SIZE) printf("ssc_wait: stat->fd=%d,last_fd=%d,last_count=%d\n",stat->fd,last_fd,last_count);
///**/			retval = ia64_ssc(arg0,0,0,0,ssc);
/**/			retval = 0;
		}
		else retval = -1L;
		vcpu_set_gr(current,8,retval);
		break;
	    case SSC_OPEN:
		arg1 = vcpu_get_gr(current,33);	// access rights
if (!running_on_sim) { printf("SSC_OPEN, not implemented on hardware.  (ignoring...)\n"); arg0 = 0; }
		if (arg0) {	// metaphysical address
			arg0 = translate_domain_mpaddr(arg0);
			retval = ia64_ssc(arg0,arg1,0,0,ssc);
		}
		else retval = -1L;
		vcpu_set_gr(current,8,retval);
		break;
	    case SSC_WRITE:
	    case SSC_READ:
//if (ssc == SSC_WRITE) printf("DOING AN SSC_WRITE\n");
		arg1 = vcpu_get_gr(current,33);
		arg2 = vcpu_get_gr(current,34);
		arg3 = vcpu_get_gr(current,35);
		if (arg2) {	// metaphysical address of descriptor
			struct ssc_disk_req *req;
			unsigned long mpaddr, paddr;
			long len;

			arg2 = translate_domain_mpaddr(arg2);
			req = (struct disk_req *)__va(arg2);
			req->len &= 0xffffffffL;	// avoid strange bug
			len = req->len;
/**/			last_fd = arg1;
/**/			last_count = len;
			mpaddr = req->addr;
//if (last_count >= PAGE_SIZE) printf("do_ssc: read fd=%d, addr=%p, len=%lx ",last_fd,mpaddr,len);
			retval = 0;
			if ((mpaddr & PAGE_MASK) != ((mpaddr+len-1) & PAGE_MASK)) {
				// do partial page first
				req->addr = translate_domain_mpaddr(mpaddr);
				req->len = PAGE_SIZE - (req->addr & ~PAGE_MASK);
				len -= req->len; mpaddr += req->len;
				retval = ia64_ssc(arg0,arg1,arg2,arg3,ssc);
				arg3 += req->len; // file offset
/**/				last_stat.fd = last_fd;
/**/				(void)ia64_ssc(__pa(&last_stat),0,0,0,SSC_WAIT_COMPLETION);
//if (last_count >= PAGE_SIZE) printf("ssc(%p,%lx)[part]=%x ",req->addr,req->len,retval);
			}
			if (retval >= 0) while (len > 0) {
				req->addr = translate_domain_mpaddr(mpaddr);
				req->len = (len > PAGE_SIZE) ? PAGE_SIZE : len;
				len -= PAGE_SIZE; mpaddr += PAGE_SIZE;
				retval = ia64_ssc(arg0,arg1,arg2,arg3,ssc);
				arg3 += req->len; // file offset
// TEMP REMOVED AGAIN				arg3 += req->len; // file offset
/**/				last_stat.fd = last_fd;
/**/				(void)ia64_ssc(__pa(&last_stat),0,0,0,SSC_WAIT_COMPLETION);
//if (last_count >= PAGE_SIZE) printf("ssc(%p,%lx)=%x ",req->addr,req->len,retval);
			}
			// set it back to the original value
			req->len = last_count;
		}
		else retval = -1L;
		vcpu_set_gr(current,8,retval);
//if (last_count >= PAGE_SIZE) printf("retval=%x\n",retval);
		break;
	    case SSC_CONNECT_INTERRUPT:
		arg1 = vcpu_get_gr(current,33);
		arg2 = vcpu_get_gr(current,34);
		arg3 = vcpu_get_gr(current,35);
		if (!running_on_sim) { printf("SSC_CONNECT_INTERRUPT, not implemented on hardware.  (ignoring...)\n"); break; }
		(void)ia64_ssc(arg0,arg1,arg2,arg3,ssc);
		break;
	    case SSC_NETDEV_PROBE:
		vcpu_set_gr(current,8,-1L);
		break;
	    default:
		printf("ia64_handle_break: bad ssc code %lx\n",ssc);
		break;
	}
	vcpu_increment_iip(current);
}

void
ia64_handle_break (unsigned long ifa, struct pt_regs *regs, unsigned long isr, unsigned long iim)
{
	static int first_time = 1;
	struct domain *d = (struct domain *) current->domain;
	struct exec_domain *ed = (struct domain *) current;
	extern unsigned long running_on_sim;

	if (first_time) {
		if (platform_is_hp_ski()) running_on_sim = 1;
		else running_on_sim = 0;
		first_time = 0;
	}
	if (iim == 0x80001 || iim == 0x80002) {	//FIXME: don't hardcode constant
		if (running_on_sim) do_ssc(vcpu_get_gr(current,36), regs);
		else do_ssc(vcpu_get_gr(current,36), regs);
	}
	else if (iim == d->breakimm) {
		if (ia64_hypercall(regs))
			vcpu_increment_iip(current);
	}
	else reflect_interruption(ifa,isr,iim,regs,IA64_BREAK_VECTOR);
}

void
ia64_handle_privop (unsigned long ifa, struct pt_regs *regs, unsigned long isr, unsigned long itir)
{
	IA64FAULT vector;
	struct domain *d = current->domain;
	struct exec_domain *ed = current;
	// FIXME: no need to pass itir in to this routine as we need to
	// compute the virtual itir anyway (based on domain's RR.ps)
	// AND ACTUALLY reflect_interruption doesn't use it anyway!
	itir = vcpu_get_itir_on_fault(ed,ifa);
	vector = priv_emulate(current,regs,isr);
	if (vector == IA64_RETRY) {
		reflect_interruption(ifa,isr,itir,regs,
			IA64_ALT_DATA_TLB_VECTOR | IA64_FORCED_IFA);
	}
	else if (vector != IA64_NO_FAULT && vector != IA64_RFI_IN_PROGRESS) {
		reflect_interruption(ifa,isr,itir,regs,vector);
	}
}

#define INTR_TYPE_MAX	10
UINT64 int_counts[INTR_TYPE_MAX];

void
ia64_handle_reflection (unsigned long ifa, struct pt_regs *regs, unsigned long isr, unsigned long iim, unsigned long vector)
{
	struct domain *d = (struct domain *) current->domain;
	struct exec_domain *ed = (struct domain *) current;
	unsigned long check_lazy_cover = 0;
	unsigned long psr = regs->cr_ipsr;
	unsigned long itir = vcpu_get_itir_on_fault(ed,ifa);

	if (!(psr & IA64_PSR_CPL)) {
		printk("ia64_handle_reflection: reflecting with priv=0!!\n");
	}
	// FIXME: no need to pass itir in to this routine as we need to
	// compute the virtual itir anyway (based on domain's RR.ps)
	// AND ACTUALLY reflect_interruption doesn't use it anyway!
	itir = vcpu_get_itir_on_fault(ed,ifa);
	switch(vector) {
	    case 8:
		vector = IA64_DIRTY_BIT_VECTOR; break;
	    case 9:
		vector = IA64_INST_ACCESS_BIT_VECTOR; break;
	    case 10:
		check_lazy_cover = 1;
		vector = IA64_DATA_ACCESS_BIT_VECTOR; break;
	    case 20:
		check_lazy_cover = 1;
		vector = IA64_PAGE_NOT_PRESENT_VECTOR; break;
	    case 22:
		vector = IA64_INST_ACCESS_RIGHTS_VECTOR; break;
	    case 23:
		check_lazy_cover = 1;
		vector = IA64_DATA_ACCESS_RIGHTS_VECTOR; break;
	    case 25:
		vector = IA64_DISABLED_FPREG_VECTOR; break;
	    case 26:
printf("*** NaT fault... attempting to handle as privop\n");
		vector = priv_emulate(ed,regs,isr);
		if (vector == IA64_NO_FAULT) {
printf("*** Handled privop masquerading as NaT fault\n");
			return;
		}
		vector = IA64_NAT_CONSUMPTION_VECTOR; break;
	    case 27:
//printf("*** Handled speculation vector, itc=%lx!\n",ia64_get_itc());
		itir = iim;
		vector = IA64_SPECULATION_VECTOR; break;
	    case 30:
		// FIXME: Should we handle unaligned refs in Xen??
		vector = IA64_UNALIGNED_REF_VECTOR; break;
	    default:
		printf("ia64_handle_reflection: unhandled vector=0x%lx\n",vector);
		while(vector);
		return;
	}
	if (check_lazy_cover && handle_lazy_cover(ed, isr, regs)) return;
	reflect_interruption(ifa,isr,itir,regs,vector);
}
