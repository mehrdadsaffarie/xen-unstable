/*
 * include/asm-i386/i387.h
 *
 * Copyright (C) 1994 Linus Torvalds
 *
 * Pentium III FXSR, SSE support
 * General FPU state handling cleanups
 *	Gareth Hughes <gareth@valinux.com>, May 2000
 */

#ifndef __ASM_I386_I387_H
#define __ASM_I386_I387_H

#include <xeno/sched.h>
#include <asm/processor.h>

extern void init_fpu(void);
extern void save_init_fpu( struct task_struct *tsk );
extern void restore_fpu( struct task_struct *tsk );

#define unlazy_fpu( tsk ) do { \
	if ( tsk->flags & PF_USEDFPU ) \
		save_init_fpu( tsk ); \
} while (0)

#define clear_fpu( tsk ) do { \
	if ( tsk->flags & PF_USEDFPU ) { \
		asm volatile("fwait"); \
		tsk->flags &= ~PF_USEDFPU; \
		stts(); \
	} \
} while (0)

#define load_mxcsr( val ) do { \
        unsigned long __mxcsr = ((unsigned long)(val) & 0xffbf); \
        asm volatile( "ldmxcsr %0" : : "m" (__mxcsr) ); \
} while (0)

#endif /* __ASM_I386_I387_H */
