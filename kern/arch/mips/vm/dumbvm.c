/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>

#include "opt-A3.h"
/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground.
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

/*
 * Globals
 */
#if OPT_A3
	struct coremap *coremap_table;
	int frame_num = 0;
	bool vm_booted = false;
#endif

void
vm_bootstrap(void)
{
#if OPT_A3
	DEBUG(DB_MEMORY, "************virtual memory booting************\n");
	/*get remaining physical memory and partition to each frame*/

	// lo is first physical addr. hi is last
	paddr_t lo, hi, paddr_i;
	ram_getsize(&lo, &hi);

	//page size is 4kb(4096byte)
	frame_num = (hi - lo) / PAGE_SIZE;
	paddr_t actual_lo = lo + frame_num * (sizeof(struct coremap));
	actual_lo = ROUNDUP(actual_lo, PAGE_SIZE);
	DEBUG(DB_MEMORY, "frame_num is %d\n", frame_num);
	DEBUG(DB_MEMORY, "first physical address is %d\n", lo);
	DEBUG(DB_MEMORY, "actual first physical address is %d\n", actual_lo);
	DEBUG(DB_MEMORY, "last physical address is %d\n", hi);
	DEBUG(DB_MEMORY, "--------------------------------\n");

	/*keep track of status of each frame*/
	paddr_i = actual_lo;
	frame_num = (hi - actual_lo) / PAGE_SIZE;
	DEBUG(DB_MEMORY, "new frame_num is %d\n", frame_num);
	coremap_table = (struct coremap*)PADDR_TO_KVADDR(lo);

	for (int i=0; i<frame_num; i++) {
		//DEBUG(DB_MEMORY, "paddr_i is %d\n", (int)paddr_i);
		coremap_table[i].paddr = paddr_i;
		coremap_table[i].available = true;
		coremap_table[i].contiguous = false;
		coremap_table[i].contiguous_frame_num = 0;
		paddr_i += PAGE_SIZE;
	}
	vm_booted = true;
#endif
}

static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;
#if OPT_A3
	if (!vm_booted) {
		spinlock_acquire(&stealmem_lock);

		addr = ram_stealmem(npages);

		spinlock_release(&stealmem_lock);
		return addr;
	} else {
		//should not call ram_stealmen again
		//DEBUG(DB_MEMORY, "----------getting page after booted----------\n");
		//DEBUG(DB_MEMORY, "Need to find %d contiguous frames\n", (int) npages);

		spinlock_acquire(&stealmem_lock);

		int i=0;
		unsigned long first_frame = 0;
		bool contiguous_block = true;

		while(i < frame_num) {

			if (coremap_table[i].available) {
				first_frame = i;
				for (unsigned long j=1; j<npages; j++) {
					if (!(coremap_table[i+j].available)) {
						DEBUG(DB_MEMORY, "coremap_table[%d] is unavailable\n", (int)(i+j));
						contiguous_block = false;
						break;
					}
				}
				if (contiguous_block) {
					//DEBUG(DB_MEMORY, "contiguous block found, first frame is %d\n", (int)first_frame);
					for (unsigned long l=first_frame; l<npages+first_frame; l++) {
						if (l == first_frame) {
							addr = coremap_table[i].paddr;
							coremap_table[l].contiguous_frame_num = npages;
						}
						coremap_table[l].available = false;
						coremap_table[l].contiguous = true;
					}
					//DEBUG(DB_MEMORY, "Calling break;\n");
					break;
				}
			}

			i++;
			//DEBUG(DB_MEMORY, "Still in while loop;\n");
		}
		if (!contiguous_block){
			DEBUG(DB_MEMORY, "contiguous_block not found\n");
			addr = 0;
		}
		spinlock_release(&stealmem_lock);
		//DEBUG(DB_MEMORY, "addr to return is %d\n", (int)addr);
		return addr;
	}
#else
	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);

	spinlock_release(&stealmem_lock);
	return addr;
#endif
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t
alloc_kpages(int npages)
{
	paddr_t pa;
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void
free_kpages(vaddr_t addr)
{
#if OPT_A3
	DEBUG(DB_MEMORY, "********freeing kpages********\n");
	spinlock_acquire(&stealmem_lock);
	int i=0;
	//DEBUG(DB_MEMORY, "frame_num is %d\n", frame_num);
	DEBUG(DB_MEMORY, "addr is %d\n", (int) addr);
	while(i < frame_num) {
		//DEBUG(DB_MEMORY, "coremap_table[i].paddr is %d\n", (int) coremap_table[i].paddr);
		if (coremap_table[i].paddr == addr) {
			int contiguous_num = coremap_table[i].contiguous_frame_num;
			for (int j=0; j<contiguous_num; j++) {
				coremap_table[i+j].available = true;
				coremap_table[i+j].contiguous = false;
				coremap_table[i+j].contiguous_frame_num = 0;
				DEBUG(DB_MEMORY, "frame %d is freed\n", i+j);
			}
			break;
		}
		i++;
	}
	spinlock_release(&stealmem_lock);
#else
	/* nothing - leak the memory. */

	(void)addr;
#endif
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	//DEBUG(DB_MEMORY, "!---------------vm_fault---------------!\n");
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
#if OPT_A3
	    /* don't panic, kill the process */
	    return 1;
#else
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
#endif
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}
#if OPT_A3

	KASSERT(as->page_table1 != NULL);
	KASSERT(as->page_table2 != NULL);
	KASSERT(as->stack_page_table != NULL);

	for (size_t i=0; i<as->as_npages1; i++) {
		KASSERT((as->page_table1[i] & PAGE_FRAME) == as->page_table1[i]);
	}
	for (size_t i=0; i<as->as_npages2; i++) {
		KASSERT((as->page_table2[i] & PAGE_FRAME) == as->page_table2[i]);
	}
	for (size_t i=0; i<DUMBVM_STACKPAGES; i++) {
		KASSERT((as->stack_page_table[i] & PAGE_FRAME) == as->stack_page_table[i]);
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);
#endif

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		paddr = (faultaddress - vbase1) + as->as_pbase1;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
#if OPT_A3
		/*if segment is in text and load_elf is completed,
		 *set TLBLO_DIRT to false
		 */
		bool load_complete = as->load_elf_complete;
		if (faultaddress >= vbase1 && faultaddress < vtop1 && load_complete) {
			elo&=~TLBLO_DIRTY;
		}
#endif
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}
#if OPT_A3
	//handle TLB Fault
	ehi = faultaddress;
	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	bool load_complete = as->load_elf_complete;
	if (faultaddress >= vbase1 && faultaddress < vtop1 && load_complete) {
		elo&=~TLBLO_DIRTY;
	}
	tlb_random(ehi,elo);
	splx(spl);
	return 0;
#endif
	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
}

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}
#if OPT_A3
	as->load_elf_complete = false;

	as->as_vbase1 = 0;
	as->page_table1 = NULL;
	as->as_npages1 = 0;
	as->page_table1_readable = true;
	as->page_table1_writeable = true;
	as->page_table1_executable = true;

	as->as_vbase2 = 0;
	as->page_table2 = NULL;
	as->as_npages2 = 0;
	as->page_table2_readable = false;
	as->page_table2_writeable = false;
	as->page_table2_executable = false;

	as->stack_page_table = NULL;
#else
	as->as_vbase1 = 0;
	as->as_pbase1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_pbase2 = 0;
	as->as_npages2 = 0;
	as->as_stackpbase = 0;

#endif
	return as;
}

void
as_destroy(struct addrspace *as)
{
#if OPT_A3
	DEBUG(DB_MEMORY, "!---------------as_destroy---------------!\n");
	for (size_t i=0; i<as->as_npages1; i++) {
		//DEBUG(DB_MEMORY, "as->page_table1[%d] is %d\n", i, (int)as->page_table1[i]);
		free_kpages(as->page_table1[i]);
	}
	for (size_t i=0; i<as->as_npages2; i++) {
		//DEBUG(DB_MEMORY, "as->page_table2[%d] is %d\n", i, (int)as->page_table2[i]);
		free_kpages(as->page_table2[i]);
	}
	for (size_t i=0; i<DUMBVM_STACKPAGES; i++) {
		//DEBUG(DB_MEMORY, "as->stack_page_table[%d] is %d\n", i, (int)as->stack_page_table[i]);
		free_kpages(as->stack_page_table[i]);
	}
	kfree(as->page_table1);
	kfree(as->page_table2);
	kfree(as->stack_page_table);
	kfree(as);
#else
	kfree(as);
#endif
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/* nothing */
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	DEBUG(DB_MEMORY, "!---------------as_define_region---------------!\n");
	DEBUG(DB_MEMORY, "writeable %d\n", writeable);
	DEBUG(DB_MEMORY, "readable %d\n", readable);
	DEBUG(DB_MEMORY, "executable %d\n", executable);
	size_t npages;

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;
	DEBUG(DB_MEMORY, "npages %d\n", npages);
#if OPT_A3
	if (as->stack_page_table == NULL) {
		as->stack_page_table = kmalloc(DUMBVM_STACKPAGES * sizeof(paddr_t));
		DEBUG(DB_MEMORY, "stack_page_table created!\n");
	}
	if (as->page_table1 == NULL) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		as->page_table1 = kmalloc(npages * sizeof(paddr_t));
		as->page_table1_readable = readable;
		as->page_table1_writeable = writeable;
		as->page_table1_executable = executable;
		DEBUG(DB_MEMORY, "page_table1 created! %d pages\n", npages);
		return 0;
	}
	if (as->page_table2 == NULL) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		as->page_table2 = kmalloc(npages * sizeof(paddr_t));
		as->page_table2_readable = readable;
		as->page_table2_writeable = writeable;
		as->page_table2_executable = executable;
		DEBUG(DB_MEMORY, "page_table2 created! %d pages\n", npages);
		return 0;
	}

#else
	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		return 0;
	}
#endif
	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

int
as_prepare_load(struct addrspace *as)
{
#if OPT_A3
	DEBUG(DB_MEMORY, "!---------------as_prepare_load---------------!\n");
	KASSERT(as->page_table1 != NULL);
	KASSERT(as->page_table2 != NULL);
	//KASSERT(as->stack_page_table != NULL);

	for (size_t i=0; i<as->as_npages1; i++) {
		as->page_table1[i] = getppages(1);
		//DEBUG(DB_MEMORY, "as->page_table1[%d] is %d\n", i, (int)as->page_table1[i]);
		if (i==0) {
			as->as_pbase1 = as->page_table1[i];
		}
		if (as->page_table1[i] == 0) {
			return ENOMEM;
		}
		as_zero_region(as->page_table1[i], 1);
	}
	DEBUG(DB_MEMORY, "page_table1 loaded\n");
	for (size_t i=0; i<as->as_npages2; i++) {
		as->page_table2[i] = getppages(1);
		//DEBUG(DB_MEMORY, "as->page_table2[%d] is %d\n", i, (int)as->page_table2[i]);
		if (i==0) {
			as->as_pbase2 = as->page_table2[i];
		}
		if (as->page_table2[i] == 0) {
			return ENOMEM;
		}
		as_zero_region(as->page_table2[i], 1);
	}
	DEBUG(DB_MEMORY, "page_table2 loaded\n");
	for (int i=0; i<DUMBVM_STACKPAGES; i++) {
		as->stack_page_table[i] = getppages(1);
		//DEBUG(DB_MEMORY, "as->stack_page_table[%d] is %d\n", i, (int)as->stack_page_table[i]);
		if (i==0) {
			as->as_stackpbase = as->stack_page_table[i];
		}
		if (as->stack_page_table[i] == 0) {
			return ENOMEM;
		}
		as_zero_region(as->stack_page_table[i], 1);
	}
#else
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);

	as->as_pbase1 = getppages(as->as_npages1);
	if (as->as_pbase1 == 0) {
		return ENOMEM;
	}

	as->as_pbase2 = getppages(as->as_npages2);
	if (as->as_pbase2 == 0) {
		return ENOMEM;
	}

	as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
	if (as->as_stackpbase == 0) {
		return ENOMEM;
	}

	as_zero_region(as->as_pbase1, as->as_npages1);
	as_zero_region(as->as_pbase2, as->as_npages2);
	as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);
#endif
	DEBUG(DB_MEMORY, "!---------------as_prepare_load return---------------!\n");
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
#if OPT_A3
	/*
	*indicate load_elf has completed
	*flush the TLB
	*/
	as->load_elf_complete = true;
	as_activate();
	return 0;
#else
	(void)as;
	return 0;
#endif
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{

	DEBUG(DB_MEMORY, "!---------------as_define_stack---------------!\n");
	KASSERT(as->as_stackpbase != 0);

	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	DEBUG(DB_MEMORY, "!---------------as_copy---------------!\n");
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;

#if OPT_A3
	new->page_table1_readable   = old->page_table1_readable;
	new->page_table1_writeable  = old->page_table1_writeable;
	new->page_table1_executable = old->page_table1_executable;
	new->page_table2_readable   = old->page_table2_readable;
	new->page_table2_writeable  = old->page_table2_writeable;
	new->page_table2_executable = old->page_table2_executable;
#endif

#if OPT_A3
	if (new->stack_page_table == NULL) {
		new->stack_page_table = kmalloc(DUMBVM_STACKPAGES * sizeof(paddr_t));
		DEBUG(DB_MEMORY, "stack_page_table copy created!\n");
	}
	if (new->page_table1 == NULL) {
		new->page_table1 = kmalloc(new->as_npages1 * sizeof(paddr_t));
		DEBUG(DB_MEMORY, "page_table1 copy created! %d pages\n", new->as_npages1);
	}
	if (new->page_table2 == NULL) {
		new->page_table2 = kmalloc(new->as_npages2 * sizeof(paddr_t));
		DEBUG(DB_MEMORY, "page_table2 copy created! %d pages\n", new->as_npages1);
	}
#endif

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

#if OPT_A3
	KASSERT(new->page_table1 != NULL);
	KASSERT(new->page_table2 != NULL);
	KASSERT(new->stack_page_table != NULL);
#endif

	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);
	KASSERT(new->as_stackpbase != 0);
#if OPT_A3
	for (size_t i=0; i<new->as_npages1; i++) {
		memmove((void *)PADDR_TO_KVADDR(new->page_table1[i]),
				(const void *)PADDR_TO_KVADDR(old->page_table1[i]),
					old->as_npages1*PAGE_SIZE);
	}

	for (size_t i=0; i<new->as_npages2; i++) {
		memmove((void *)PADDR_TO_KVADDR(new->page_table2[i]),
				(const void *)PADDR_TO_KVADDR(old->page_table2[i]),
					old->as_npages2*PAGE_SIZE);
	}

	for (size_t i=0; i<DUMBVM_STACKPAGES; i++) {
		memmove((void *)PADDR_TO_KVADDR(new->stack_page_table[i]),
				(const void *)PADDR_TO_KVADDR(old->stack_page_table[i]),
					DUMBVM_STACKPAGES*PAGE_SIZE);
	}
#else
	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
		(const void *)PADDR_TO_KVADDR(old->as_pbase1),
		old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
		(const void *)PADDR_TO_KVADDR(old->as_pbase2),
		old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
		(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
		DUMBVM_STACKPAGES*PAGE_SIZE);
 #endif

	*ret = new;
	return 0;
}
