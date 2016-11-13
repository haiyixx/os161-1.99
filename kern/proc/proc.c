/*
 * Copyright (c) 2013
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

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */
#include "opt-A2.h"
#define PROCINLINE

#include <types.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <vfs.h>
#include <synch.h>
#include <kern/fcntl.h>



/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

/*
 * Mechanism for making the kernel menu thread sleep while processes are running
 */
#ifdef UW
/* count of the number of processes, excluding kproc */
static volatile unsigned int proc_count;
/* provides mutual exclusion for proc_count */
/* it would be better to use a lock here, but we use a semaphore because locks are not implemented in the base kernel */
static struct semaphore *proc_count_mutex;
/* used to signal the kernel menu thread when there are no processes */
struct semaphore *no_proc_sem;
#endif  // UW



/*
 * Create a proc structure.
 */
static
struct proc *
proc_create(const char *name)
{
	struct proc *proc;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL) {
		return NULL;
	}
	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL) {
		kfree(proc);
		return NULL;
	}

	threadarray_init(&proc->p_threads);
	spinlock_init(&proc->p_lock);

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;

#ifdef UW
	proc->console = NULL;
#endif // UW

#if OPT_A2
	//lock_acquire(pid_pool_lock);
	//proc->pid       = assign_pid();
	///lock_release(pid_pool_lock);
	proc->can_exit  = false;
	proc->exit_code = 0;
	procarray_init(&proc->child_proc);

	proc->child_proc_lock = lock_create(name);
	proc->parent_proc     = NULL;
	proc->wait_pid_lock   = lock_create(name);
	proc->wait_pid_cv     = cv_create(name);
#endif
	return proc;
}


/*
 * Destroy a proc structure.
 */
void
proc_destroy(struct proc *proc)
{
	/*
         * note: some parts of the process structure, such as the address space,
         *  are destroyed in sys_exit, before we get here
         *
         * note: depending on where this function is called from, curproc may not
         * be defined because the calling thread may have already detached itself
         * from the process.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);
	DEBUG(DB_SYSCALL,"Proc_destroy: process %d \n",proc->pid);
	DEBUG(DB_SYSCALL,"Proc_destroy: process name  %s \n",proc->p_name);
#if OPT_A2
	//TODO: better structure, it's messy now
	//spinlock_acquire(proc->p_lock);
	struct proc *parent_proc = proc->parent_proc;

	// if the process has no parent, we can delete it safely
	if (parent_proc == NULL) {

		DEBUG(DB_SYSCALL,"Proc_destroy: %d 's parent is null\n",proc->pid);
		lock_acquire(proc->child_proc_lock);
		int len = procarray_num(&proc->child_proc);
		DEBUG(DB_SYSCALL,"proc %d has %d child len: \n",proc->pid, len);
		for (int i=0; i<len; i++) {
			struct proc *child = procarray_get(&proc->child_proc, i);
			spinlock_acquire(&child->p_lock);
			DEBUG(DB_SYSCALL,"Proc_destroy: proc %d has child %d \n",proc->pid,child->pid);
			if (child->can_exit) {
				//if child can exit, set the child parent to null and delete it
				child->parent_proc = NULL;
				spinlock_release(&child->p_lock);
				proc_destroy(child);
			} else {
				//child is not exitable, just set parent pointer to null
			DEBUG(DB_SYSCALL,"Proc_destroy: %d  set it's child %d \n",proc->pid, child->pid);
				child->parent_proc = NULL;
				spinlock_release(&child->p_lock);
			}
		}
		//destroy child_proc array, lock and cv
		//TODO: have error on remove, cannot figure out
		/*
		for (int i=0; i<len; i++) {
			struct proc *child = procarray_get(&proc->child_proc, i);
			DEBUG(DB_SYSCALL,"child %d will be deleted\n",child->pid);
			procarray_remove(&proc->child_proc,i);
		}
		*/
		lock_release(proc->child_proc_lock);
		lock_destroy(proc->child_proc_lock);
		lock_destroy(proc->wait_pid_lock);
		cv_destroy(proc->wait_pid_cv);


		/* VFS fields */
		if (proc->p_cwd) {
			VOP_DECREF(proc->p_cwd);
			proc->p_cwd = NULL;
		}

		if (proc->p_addrspace) {
			struct addrspace *as;

			as_deactivate();
			as = curproc_setas(NULL);
			as_destroy(as);
		}

		if (proc->console) {
	  		vfs_close(proc->console);
		}

		threadarray_cleanup(&proc->p_threads);
		spinlock_cleanup(&proc->p_lock);

		P(proc_count_mutex);
		KASSERT(proc_count > 0);
		proc_count--;
		/* signal the kernel menu thread if the process count has reached zero */
		if (proc_count == 0) {
	    	V(no_proc_sem);
		}
		V(proc_count_mutex);


		//DEBUG(DB_SYSCALL,"pid to add is : process %d \n",(int)proc->pid);
		add_pid_pool(proc->pid);
		//lock_acquire(pid_pool_lock);
		//DEBUG(DB_SYSCALL,"the address is  %d \n", &(proc->pid));
		//array_add(pid_pool, &proc->pid, NULL);
		//lock_release(pid_pool_lock);
		DEBUG(DB_SYSCALL,"process %d is deleted \n",proc->pid);
		kfree(proc->p_name);
		kfree(proc);
	}

	//delete child_proc array


#else
	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}


#ifndef UW  // in the UW version, space destruction occurs in sys_exit, not here
	if (proc->p_addrspace) {
		/*
		 * In case p is the currently running process (which
		 * it might be in some circumstances, or if this code
		 * gets moved into exit as suggested above), clear
		 * p_addrspace before calling as_destroy. Otherwise if
		 * as_destroy sleeps (which is quite possible) when we
		 * come back we'll be calling as_activate on a
		 * half-destroyed address space. This tends to be
		 * messily fatal.
		 */
		struct addrspace *as;

		as_deactivate();
		as = curproc_setas(NULL);
		as_destroy(as);
	}
#endif // UW

#ifdef UW
	if (proc->console) {
	  vfs_close(proc->console);
	}
#endif // UW

	threadarray_cleanup(&proc->p_threads);
	spinlock_cleanup(&proc->p_lock);



#ifdef UW
	/* decrement the process count */
        /* note: kproc is not included in the process count, but proc_destroy
	   is never called on kproc (see KASSERT above), so we're OK to decrement
	   the proc_count unconditionally here */
	P(proc_count_mutex);
	KASSERT(proc_count > 0);
	proc_count--;
	/* signal the kernel menu thread if the process count has reached zero */
	if (proc_count == 0) {
	  V(no_proc_sem);
	}
	V(proc_count_mutex);
#endif // UW

#endif //OPT_A2


}

/*
 * Create the process structure for the kernel.
 */
void
proc_bootstrap(void)
{
#if OPT_A2
  //create pid_pool, not working now
  DEBUG(DB_SYSCALL,"proc_bootstrap: starting\n");
  process_id = PID_MIN;
  pid_pool = array_create();
  array_init(pid_pool);
  //KASSERT(pid_pool != NULL);
  pid_pool_lock = lock_create("pid_pool_lock");
  if (pid_pool_lock == NULL) {
  	panic("could not create pid pool lock");
  }
  DEBUG(DB_SYSCALL,"proc_bootstrap: pid_pool generated\n");
#endif
  kproc = proc_create("[kernel]");
  if (kproc == NULL) {
    panic("proc_create for kproc failed\n");
  }
#ifdef UW
  proc_count = 0;
  proc_count_mutex = sem_create("proc_count_mutex",1);
  if (proc_count_mutex == NULL) {
    panic("could not create proc_count_mutex semaphore\n");
  }
  no_proc_sem = sem_create("no_proc_sem",0);
  if (no_proc_sem == NULL) {
    panic("could not create no_proc_sem semaphore\n");
  }
#endif // UW
}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *proc;
	char *console_path;

	proc = proc_create(name);
	if (proc == NULL) {
		return NULL;
	}

//have to assign pid here or thread is interrupted
#if OPT_A2
	proc->pid = assign_pid();
#endif

#ifdef UW
	/* open the console - this should always succeed */
	console_path = kstrdup("con:");
	if (console_path == NULL) {
	  panic("unable to copy console path name during process creation\n");
	}
	if (vfs_open(console_path,O_WRONLY,0,&(proc->console))) {
	  panic("unable to open the console during process creation\n");
	}
	kfree(console_path);
#endif // UW

	/* VM fields */

	proc->p_addrspace = NULL;

	/* VFS fields */

#ifdef UW
	/* we do not need to acquire the p_lock here, the running thread should
           have the only reference to this process */
        /* also, acquiring the p_lock is problematic because VOP_INCREF may block */
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		proc->p_cwd = curproc->p_cwd;
	}
#else // UW
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		proc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);
#endif // UW

#ifdef UW
	/* increment the count of processes */
        /* we are assuming that all procs, including those created by fork(),
           are created using a call to proc_create_runprogram  */
	P(proc_count_mutex);
	proc_count++;
	V(proc_count_mutex);
#endif // UW

	return proc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
	int result;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	result = threadarray_add(&proc->p_threads, t, NULL);
	spinlock_release(&proc->p_lock);
	if (result) {
		return result;
	}
	t->t_proc = proc;
	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 */
void
proc_remthread(struct thread *t)
{
	struct proc *proc;
	unsigned i, num;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	/* ugh: find the thread in the array */
	num = threadarray_num(&proc->p_threads);
	for (i=0; i<num; i++) {
		if (threadarray_get(&proc->p_threads, i) == t) {
			threadarray_remove(&proc->p_threads, i);
			spinlock_release(&proc->p_lock);
			t->t_proc = NULL;
			return;
		}
	}
	/* Did not find it. */
	spinlock_release(&proc->p_lock);
	panic("Thread (%p) has escaped from its process (%p)\n", t, proc);
}

/*
 * Fetch the address space of the current process. Caution: it isn't
 * refcounted. If you implement multithreaded processes, make sure to
 * set up a refcount scheme or some other method to make this safe.
 */
struct addrspace *
curproc_getas(void)
{
	struct addrspace *as;
#ifdef UW
        /* Until user processes are created, threads used in testing
         * (i.e., kernel threads) have no process or address space.
         */
	if (curproc == NULL) {
		return NULL;
	}
#endif

	spinlock_acquire(&curproc->p_lock);
	as = curproc->p_addrspace;
	spinlock_release(&curproc->p_lock);
	return as;
}

/*
 * Change the address space of the current process, and return the old
 * one.
 */
struct addrspace *
curproc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}

#if OPT_A2
pid_t assign_pid()
{
	KASSERT(pid_pool_lock != NULL);
	//TODO: strange error, fix later

	lock_acquire(pid_pool_lock);
	if (array_num(pid_pool) != 0) {
		DEBUG(DB_SYSCALL, "pid_pool length is %d \n", array_num(pid_pool));
		pid_t *pid = array_get(pid_pool, 0);
		pid_t retval = *pid;
		array_remove(pid_pool, 0);
		kfree(pid);
		DEBUG(DB_SYSCALL, "pid is %d \n", retval);
		DEBUG(DB_SYSCALL, "pid_pool length is %d \n", array_num(pid_pool));
		lock_release(pid_pool_lock);
		return retval;
	}

	pid_t retval = process_id;
	process_id = process_id + 1;
	DEBUG(DB_SYSCALL, "pid is %d\n", retval);
	lock_release(pid_pool_lock);
	return retval;
}

void add_pid_pool(pid_t pid)
{
	DEBUG(DB_SYSCALL,"**********add_pid_pool:  pid is %d**********\n",(int)pid);
	pid_t *p = kmalloc(sizeof(pid_t));
	*p = pid;
	lock_acquire(pid_pool_lock);
	array_add(pid_pool, p, NULL);
	lock_release(pid_pool_lock);
	//TEST
	int n = array_num(pid_pool);
	for(int i=0; i < n; i++) {
		pid_t *num = array_get(pid_pool, i);
		DEBUG(DB_SYSCALL,"pid is %d\n",(int)*num);
	}
}
#endif
