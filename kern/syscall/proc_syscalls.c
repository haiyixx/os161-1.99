#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>

#include "opt-A2.h"
#include <mips/trapframe.h>
#include <kern/fcntl.h>
#include <vfs.h>

#if OPT_A2
void pre_enter_forked_process(void *data1, unsigned long data2)
{
  //cast void * to trapframe * and slient the unused parameter
  (void)data2;
  struct trapframe *tf = data1;
  enter_forked_process(tf);
}

int sys_fork(struct trapframe *tf, pid_t *retval)
{
  KASSERT(curproc != NULL);
  //create process structure for childprocess
  struct proc *child_process = proc_create_runprogram(curproc->p_name);
  if (child_process == NULL) {
    DEBUG(DB_SYSCALL, "sys_fork: child process has no memory!\n");
    return ENOMEM;
  }
  DEBUG(DB_SYSCALL, "sys_fork: child process created\n");

  //create and copy address space
  struct addrspace *child_addrspace;
  int ascopy_retval = as_copy(curproc_getas(), &child_addrspace);
  if (ascopy_retval) {
    proc_destroy(child_process);
    return ascopy_retval;
  }
  DEBUG(DB_SYSCALL, "sys_fork: addrspace created\n");
  spinlock_acquire(&child_process->p_lock);
  child_process->p_addrspace = child_addrspace;
  spinlock_release(&child_process->p_lock);

  //assign PId, parent_proc and add child to child_proc
  *retval = child_process->pid;
  child_process->parent_proc = curproc;
  procarray_add(&curproc->child_proc, child_process, NULL);
  DEBUG(DB_SYSCALL, "sys_fork: pid is %d \n",child_process->pid);

  //create thread
  struct trapframe *child_trapframe = kmalloc(sizeof(struct trapframe));
  if (child_trapframe == NULL) {
    as_destroy(child_addrspace);
    proc_destroy(child_process);
    return ENOMEM;
  }
  DEBUG(DB_SYSCALL, "sys_fork: trapframe is created\n");
  memcpy(child_trapframe, tf, sizeof(struct trapframe));
  int threadfork_retval = thread_fork(child_process->p_name, child_process, &pre_enter_forked_process, child_trapframe, 0);
  if (threadfork_retval) {
    as_destroy(child_addrspace);
    kfree(child_trapframe);
    proc_destroy(child_process);
    return ENOMEM;
  }
  DEBUG(DB_SYSCALL, "sys_fork: threadfork is called\n");
  return 0;
}

int sys_execv(const_userptr_t program, userptr_t args)
{
  //TODO: Remember to free memory, tons of memory leak
  DEBUG(DB_SYSEXECV, "--------------sys_execv--------------\n");

  //copy program path into kernel
  char *program_path;
  size_t len;
  program_path = kmalloc(sizeof(char) * PATH_MAX);
  if (program_path == NULL) {
    return ENOMEM;
  }
  int result = copyinstr(program, program_path, PATH_MAX, &len);
  if (result) {
    kfree(program_path);
    return result;
  }

  //count number of args and copy into kernel
  int args_num = 0;

  char **args_ptr  = (char **) args;
  while (args_ptr[args_num] != NULL) {
     DEBUG(DB_SYSEXECV, "args_ptr[%d] is %s\n", args_num, args_ptr[args_num]);
    args_num++;
  }
  DEBUG(DB_SYSEXECV, "exit the loop\n");

  //args_num++;
  /*
  userptr_t args_copy = kmalloc(sizeof(userptr_t));
  if (args_copy == NULL) {
    kfree(program_path);
    return ENOMEM;
  }
  */
  char **args_copy = kmalloc(args_num * sizeof(char *));
  /*
  result = copyin(args, args_copy, sizeof(char **));
  if (result) {
    kfree(program_path);
    kfree(args_copy);
    return result;
  }
  */

  for (int i=0; i<args_num; i++) {
    args_copy[i] = kmalloc(sizeof(char) * PATH_MAX);
    result = copyinstr((const_userptr_t)args_ptr[i], args_copy[i], PATH_MAX, &len);
    DEBUG(DB_SYSEXECV, "args_copy[%d] is %s\n", i, args_copy[i]);
    if (result) {
      kfree(program_path);
      kfree(args);
      return result;
    }
  }
   DEBUG(DB_SYSEXECV, "after copyin\n");
  args_copy[args_num] = NULL;

  struct addrspace *as;
  struct vnode *v;
  vaddr_t entrypoint, stackptr;

  /* Open the file. */
  result = vfs_open(program_path, O_RDONLY, 0, &v);
  if (result) {
    return result;
  }

  /* We should be a new process. */
  //KASSERT(curproc_getas() == NULL);

  /* Create a new address space. */
  as = as_create();
  if (as ==NULL) {
    vfs_close(v);
    return ENOMEM;
  }

  /* Switch to it and activate it. */
  struct addrspace *old_addr = curproc_getas();
  curproc_setas(as);
  as_activate();

  /* Load the executable. */
  result = load_elf(v, &entrypoint);
  if (result) {
    /* p_addrspace will go away when curproc is destroyed */
    vfs_close(v);
    return result;
  }

  /* Done with the file now. */
  vfs_close(v);

  /* Define the user stack in the address space */
  result = as_define_stack(as, &stackptr);
  if (result) {
    /* p_addrspace will go away when curproc is destroyed */
    return result;
  }

  //copy args back to user stack
  DEBUG(DB_SYSEXECV, "get here\n");
  char **args_stkptr = kmalloc(args_num * sizeof(char*));
  //vaddr_t tmp_stackptr = stackptr;
  int arg_len, aligned_len;

  for (int i=args_num-1; i>=0; i--) {
    arg_len = strlen(args_copy[i]) + 1;
    aligned_len = ROUNDUP(arg_len, 4);
    stackptr -= aligned_len;
    result = copyoutstr(args_copy[i], (userptr_t)stackptr, arg_len, NULL);
    if (result) {
      return result;
    }
    DEBUG(DB_SYSEXECV, "args[%d] is %s\n", i, args_copy[i]);
    args_stkptr[i] = (char *) stackptr;
  }
  args_stkptr[args_num] = NULL;
  //len = args_num * sizeof(char*);
  //result = copyout(args_ptr, (userptr_t)stackptr, len);
  for (int i=args_num; i>=0; i--) {
    aligned_len = ROUNDUP(sizeof(char *), 4);
    stackptr -= aligned_len;
    result = copyout(&args_stkptr[i], (userptr_t)stackptr, sizeof(char*));
    if (result) {
      return result;
    }
      //args_ptr[i] = (char *) stackptr;
  }
  //destory old addr_space
  as_destroy(old_addr);
  /* Warp to user mode. */
  enter_new_process(args_num /*argc*/, (userptr_t) stackptr /*userspace addr of argv*/,
        stackptr, entrypoint);

    /* enter_new_process does not return. */
  panic("enter_new_process returned\n");
  return EINVAL;
}
#endif


  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;
  DEBUG(DB_SYSCALL,"process %d called exit\n",p->pid);
#if OPT_A2
  //change the exit status and wake up process wait on this id
  lock_acquire(p->wait_pid_lock);
  p->can_exit = true;
  p->exit_code = _MKWAIT_EXIT(exitcode);
  cv_broadcast(p->wait_pid_cv, p->wait_pid_lock);
  lock_release(p->wait_pid_lock);
#else
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
  (void)exitcode;

#endif
  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();


  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);

  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");

}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
#if OPT_A2
  *retval = curproc->pid;
#else
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  *retval = 1;
#endif
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }

  #if OPT_A2
  if (pid < PID_MIN || pid > PID_MAX) {
    return EINVAL;
  }
  struct proc *parent_proc = curproc;
  DEBUG(DB_SYSCALL,"proc %d called wait_pid, wait on %d \n", parent_proc->pid, pid);
  //check is process call its own children
  struct proc *child_proc = NULL;
  lock_acquire(parent_proc->child_proc_lock);
  int len = procarray_num(&parent_proc->child_proc);
  for(int i=0; i<len; i++) {
    struct proc *childarray_proc = procarray_get(&parent_proc->child_proc, i);
    if (childarray_proc->pid == pid) {
      child_proc = childarray_proc;
      break;
    }
  }
  lock_release(parent_proc->child_proc_lock);
  if (child_proc == NULL) {
    return ECHILD;
  }
  //wait child_proc exit
  lock_acquire(child_proc->wait_pid_lock);
  while (child_proc->can_exit == false) {
    DEBUG(DB_SYSCALL, "sys_waitpid: parent is wait for %d to exit\n", child_proc->pid);
    cv_wait(child_proc->wait_pid_cv, child_proc->wait_pid_lock);
  }
   DEBUG(DB_SYSCALL, "sys_waitpid: parent %d is wake up \n", parent_proc->pid);
  lock_release(child_proc->wait_pid_lock);
  exitstatus = child_proc->exit_code;
  #else
  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;
  #endif

  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}

