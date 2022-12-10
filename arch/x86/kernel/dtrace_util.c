/* SPDX-License-Identifier: GPL-2.0 */
/*
 * FILE:	dtrace_util.c
 * DESCRIPTION:	Dynamic Tracing: Architecture utility functions
 *
 * Copyright (c) 2010, 2018, Oracle and/or its affiliates. All rights reserved.
 */

#include <linux/dtrace_cpu.h>
#include <linux/dtrace_os.h>
#include <linux/dtrace_task_impl.h>
#include <linux/kdebug.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/memory.h>
#include <linux/notifier.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sched/task_stack.h>
#include <asm/insn.h>
#include <asm/pgtable.h>
#include <asm/ptrace.h>
#include <asm/text-patching.h>
#include <asm/dtrace_arch.h>
#include <asm/dtrace_util.h>

int dtrace_instr_size(const asm_instr_t *addr)
{
	struct insn		insn;

	kernel_insn_init(&insn, addr, MAX_INSN_SIZE);
	insn_get_length(&insn);

	return insn_complete(&insn) ? insn.length : -1;
}
EXPORT_SYMBOL(dtrace_instr_size);

/*
 * Move the instruction pointer forward to the next instruction, effectiely
 * skipping the current one.
 */
static void dtrace_skip_instruction(struct pt_regs *regs)
{
	int	delta;

	delta = dtrace_instr_size((asm_instr_t *)regs->ip);
	BUG_ON(delta <= 0);

	regs->ip += delta;
}

void dtrace_handle_badaddr(struct pt_regs *regs)
{
	unsigned long	addr = read_cr2();

	DTRACE_CPUFLAG_SET(CPU_DTRACE_BADADDR);
	this_cpu_core->cpuc_dtrace_illval = addr;

	dtrace_skip_instruction(regs);
}

struct dtrace_invop_hdlr {
	uint8_t				(*dtih_func)(struct pt_regs *);
	struct dtrace_invop_hdlr	*dtih_next;
};

static struct dtrace_invop_hdlr	*dtrace_invop_hdlrs;

/*
 * Trap notification handler.
 */
int dtrace_die_notifier(struct notifier_block *nb, unsigned long val,
			void *args)
{
	struct die_args		*dargs = args;
	int			orig_trapnr = 0;

	switch (val) {
	case DIE_PAGE_FAULT: {
		if (!DTRACE_CPUFLAG_ISSET(CPU_DTRACE_NOFAULT))
			return NOTIFY_DONE;

		dtrace_handle_badaddr(dargs->regs);

		return NOTIFY_OK | NOTIFY_STOP_MASK;
	}
	case DIE_GPF: {
		/*
		 * This gets messy...  For one, some versions of Xen deliver
		 * the invalid opcode generated by the LOCK prefix (0xf0) as a
		 * GP fault rather than a UD fault.  So, we need to figure out
		 * whether the GP we're processing here is one of those
		 * misreported faults.
		 *
		 * But, it is possible that the instruction that caused the
		 * fault (0xf0) gets overwritten by a different CPU with the
		 * original valid opcode before we get to look at it here,
		 * which makes it kind of hard to recognize.
		 *
		 * So...  we're going to assume that a GP fault that gets
		 * triggered for the LOCK prefix opcode (0xf0) *or* for an
		 * opcode that can get overwritten with the LOCK prefix for
		 * probing is actually a UD fault.
		 *
		 * If we are wrong, the handlers will simply see a fault that
		 * isn't theirs, and return without consuming it.  And in that
		 * case, the kernel will report a UD fault that may have been
		 * a real GP fault...  Sorry.
		 */
		asm_instr_t	opc = *(asm_instr_t *)dargs->regs->ip;

		if (opc != 0xf0 && opc != 0x55 && opc != 0xc3) {
			if (!DTRACE_CPUFLAG_ISSET(CPU_DTRACE_NOFAULT))
				return NOTIFY_DONE;

			dtrace_handle_badaddr(dargs->regs);

			return NOTIFY_OK | NOTIFY_STOP_MASK;
		}

		/*
		 * ... and instead treat them as the SDT probe point traps that
		 * they are.
		 */
		orig_trapnr = dargs->trapnr;
		dargs->trapnr = 6;
	}
	case DIE_TRAP: {
		struct dtrace_invop_hdlr *hdlr;
		int			 rval = 0;

		if (dargs->trapnr != 6)
			return NOTIFY_DONE;

		for (hdlr = dtrace_invop_hdlrs; hdlr != NULL;
		     hdlr = hdlr->dtih_next) {
			rval = hdlr->dtih_func(dargs->regs);
			if (rval != 0)
				break;
		}

		switch (rval) {
		case DTRACE_INVOP_NOPS:
			/*
			 * SDT probe points are encoded as either:
			 *   - a 1-byte NOP followed by a multi-byte NOP
			 *   - a multi-byte code sequence (to set AX to 0),
			 *     followed by a multi-byte NOP
			 * In both cases, the total length of the probe point
			 * instruction is ASM_CALL_SITE bytes, so we can safely
			 * skip that number of bytes here.
			 */
			dargs->regs->ip += ASM_CALL_SIZE;
			return NOTIFY_OK | NOTIFY_STOP_MASK;
		case DTRACE_INVOP_MOV_RSP_RBP:
		case DTRACE_INVOP_NOP:
		case DTRACE_INVOP_PUSH_BP:
		case DTRACE_INVOP_RET:
			return notifier_from_errno(-rval);
		default:
			/*
			 * This must not have been a trap triggered from a
			 * probe point.  Let someone else deal with it...
			 *
			 * If we got here because of a GPF that we thought
			 * was a UD (due to a bug in some versions of Xen),
			 * undo our change to dargs->trapnr.
			 */
			if (unlikely(orig_trapnr))
				dargs->trapnr = orig_trapnr;

			return NOTIFY_DONE;
		}
	}
	case DIE_INT3: {
		struct dtrace_invop_hdlr *hdlr;
		int			 rval = 0;

		/*
		 * Let's assume that this is a DTrace probe firing, so we need
		 * to adjust the IP (to be consistent with #UD processing) so
		 * that it reflects the address of the #BP rather than the
		 * following intruction.
		 *
		 * If it turns out that this was not DTrace related, we'll have
		 * to reverse this adjustment.
		 */
		dargs->regs->ip--;
		for (hdlr = dtrace_invop_hdlrs; hdlr != NULL;
		     hdlr = hdlr->dtih_next) {
			rval = hdlr->dtih_func(dargs->regs);
			if (rval != 0)
				break;
		}

		switch (rval) {
		case DTRACE_INVOP_NOPS:
			/*
			 * SDT probe points are encoded as either:
			 *   - a 1-byte NOP followed by a multi-byte NOP
			 *   - a multi-byte code sequence (to set AX to 0),
			 *     followed by a multi-byte NOP
			 * In both cases, the total length of the probe point
			 * instruction is ASM_CALL_SITE bytes, so we can safely
			 * skip that number of bytes here.
			 */
			dargs->regs->ip += ASM_CALL_SIZE;
			return NOTIFY_OK | NOTIFY_STOP_MASK;
		case DTRACE_INVOP_MOV_RSP_RBP:
		case DTRACE_INVOP_NOP:
		case DTRACE_INVOP_PUSH_BP:
		case DTRACE_INVOP_RET:
			return notifier_from_errno(-rval);
		default:
			/*
			 * This must not have been a trap triggered from a
			 * probe point.  Re-adjust the instruction pointer
			 * and let someone else deal with it...
			 */
			dargs->regs->ip++;
		}
	}
	default:
		return NOTIFY_DONE;
	}
}

/*
 * Add an INVOP trap handler.
 */
int dtrace_invop_add(uint8_t (*func)(struct pt_regs *))
{
	struct dtrace_invop_hdlr	*hdlr;

	hdlr = kmalloc(sizeof(struct dtrace_invop_hdlr), GFP_KERNEL);
	if (hdlr == NULL) {
		pr_warn("Failed to add invop handler: out of memory\n");
		return -ENOMEM;
	}

	hdlr->dtih_func = func;
	hdlr->dtih_next = dtrace_invop_hdlrs;
	dtrace_invop_hdlrs = hdlr;

	return 0;
}
EXPORT_SYMBOL(dtrace_invop_add);

/*
 * Remove an INVOP trap handler.
 */
void dtrace_invop_remove(uint8_t (*func)(struct pt_regs *))
{
	struct dtrace_invop_hdlr *hdlr = dtrace_invop_hdlrs, *prev = NULL;

	for (;;) {
		if (hdlr == NULL)
			return;

		if (hdlr->dtih_func == func)
			break;

		prev = hdlr;
		hdlr = hdlr->dtih_next;
	}

	if (prev == NULL)
		dtrace_invop_hdlrs = hdlr->dtih_next;
	else
		prev->dtih_next = hdlr->dtih_next;

	kfree(hdlr);
}
EXPORT_SYMBOL(dtrace_invop_remove);

/*
 * Enable an INVOP-based probe, i.e. ensure that an INVOP trap is triggered at
 * the specified address.
 */
void dtrace_invop_enable(asm_instr_t *addr, asm_instr_t opcode)
{
	mutex_lock(&text_mutex);
	text_poke(addr, ((unsigned char []){opcode}), 1);
	mutex_unlock(&text_mutex);
}
EXPORT_SYMBOL(dtrace_invop_enable);

/*
 * Disable an INVOP-based probe.
 */
void dtrace_invop_disable(asm_instr_t *addr, asm_instr_t opcode)
{
	mutex_lock(&text_mutex);
	text_poke(addr, ((unsigned char []){opcode}), 1);
	mutex_unlock(&text_mutex);
}
EXPORT_SYMBOL(dtrace_invop_disable);

static inline int dtrace_bad_address(void *addr)
{
	unsigned long	dummy;

	return probe_kernel_address((unsigned long *)addr, dummy);
}

static int dtrace_user_addr_is_exec(uintptr_t addr)
{
	struct mm_struct	*mm = current->mm;
	pgd_t			*pgd;

#if CONFIG_PGTABLE_LEVELS > 3
	p4d_t			*p4d;
#endif

	pud_t			*pud;
	pmd_t			*pmd;
	pte_t			*pte;
	unsigned long		flags;
	int			ret = 0;

	if (mm == NULL)
		return 0;

	addr &= PAGE_MASK;

	local_irq_save(flags);

	pgd = pgd_offset(mm, addr);
	if (dtrace_bad_address(pgd))
		goto out;
	if (pgd_none(*pgd) || !pgd_present(*pgd))
		goto out;

#if CONFIG_PGTABLE_LEVELS > 3
	p4d = p4d_offset(pgd, addr);
	if (dtrace_bad_address(p4d))
		goto out;
	if (p4d_none(*p4d) || !p4d_present(*p4d))
		goto out;

	pud = pud_offset(p4d, addr);
#else
	pud = pud_offset(pgd, addr);
#endif

	if (dtrace_bad_address(pud))
		goto out;
	if (pud_none(*pud) || !pud_present(*pud))
		goto out;
	if (unlikely(pud_large(*pud))) {
		pte = (pte_t *)pud;
		if (dtrace_bad_address(pte))
			goto out;

		ret = pte_exec(*pte);
		goto out;
	}

	pmd = pmd_offset(pud, addr);
	if (dtrace_bad_address(pmd))
		goto out;
	if (pmd_none(*pmd))
		goto out;
	if (unlikely(pmd_large(*pmd) || !pmd_present(*pmd))) {
		pte = (pte_t *)pmd;
		if (dtrace_bad_address(pte))
			goto out;

		ret = pte_exec(*pte);
		goto out;
	}

	pte = pte_offset_map(pmd, addr);
	if (dtrace_bad_address(pte))
		goto out;
	if (pte_protnone(*pte))
		goto out;
	if ((pte_flags(*pte) & (_PAGE_PRESENT|_PAGE_USER|_PAGE_SPECIAL)) !=
	    (_PAGE_PRESENT|_PAGE_USER))
		goto out;

	ret = pte_exec(*pte);

out:
	local_irq_restore(flags);

	return ret;
}

void dtrace_user_stacktrace(struct stacktrace_state *st)
{
	struct pt_regs		*regs = current_pt_regs();
	uint64_t		*pcs = st->pcs;
	int			limit = st->limit;
	unsigned long		*bos;
	unsigned long		*sp = (unsigned long *)user_stack_pointer(regs);
	int			ret;

	if (!user_mode(regs))
		goto out;

	if (current->dt_task == NULL)
		goto out;

	bos = current->dt_task->dt_ustack;

	st->depth = 1;
	if (pcs)
		*pcs++ = (uint64_t)instruction_pointer(regs);
	limit--;

	if (!limit)
		goto out;

	while (sp <= bos && limit) {
		unsigned long	pc;

		pagefault_disable();
		ret = __copy_from_user_inatomic(&pc, sp, sizeof(pc));
		pagefault_enable();

		if (ret)
			break;

		if (dtrace_user_addr_is_exec(pc)) {
			if (pcs)
				*pcs++ = pc;
			limit--;
			st->depth++;
		}

		sp++;
	}

out:
	if (pcs) {
		while (limit--)
			*pcs++ = 0;
	}
}

void dtrace_mod_pdata_init(struct dtrace_module *pdata)
{
}

void dtrace_mod_pdata_cleanup(struct dtrace_module *pdata)
{
}