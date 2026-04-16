#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	debugf("sys_read fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDIN)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	
	// PROJECT 2: Translate virtual address to physical
	uint64 val_pa = useraddr(p->pagetable, val);  // Change val_va to val
	if (val_pa == 0) {
		return -1;
	}
	
	TimeVal *val_ptr = (TimeVal *)val_pa;  // Change variable name to val_ptr
	uint64 cycle = get_cycle();
	val_ptr->sec = cycle / CPU_FREQ;
	val_ptr->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	
	return 0;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!\n");
	return fork();
}

uint64 sys_exec(uint64 va)
{
	struct proc *p = curr_proc();
	char name[200];
	copyinstr(p->pagetable, name, va, 200);
	debugf("sys_exec %s\n", name);
	return exec(name);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_task_info(uint64 ti_va)
{
	struct proc *p = curr_proc();
	
	// Translate virtual address to physical address
	uint64 ti_pa = useraddr(p->pagetable, ti_va);
	if (ti_pa == 0) {
		return -1;
	}
	
	TaskInfo *ti = (TaskInfo *)ti_pa;
	
	// Fill in the status
	ti->status = Running;
	
	// Copy syscall counts
	for (int i = 0; i < 500; i++) {
		ti->syscall_times[i] = p->syscall_times[i];
	}
	
	// Calculate runtime since process started
	ti->time = ((get_cycle() - p->start_time) * 1000) / CPU_FREQ;
	
	return 0;
}

uint64 sys_mmap(uint64 start, uint64 len, int prot, int flags, int fd)
{
	// Validate parameters
	if (len == 0 || len > 1024 * 1024 * 1024) {
		return -1;
	}
	
	// Reject if start is not page-aligned
	if (start != PGROUNDDOWN(start)) {
		return -1;
	}
	
	// Validate prot bits (only bits 0-2 should be set)
	if ((prot & ~0x7) != 0) {
		return -1;
	}
	
	// Must have at least one permission bit set
	if ((prot & 0x7) == 0) {
		return -1;
	}
	
	struct proc *p = curr_proc();
	
	// Convert prot bits to PTE permission flags
	int perm = PTE_U;
	if (prot & 0x1) perm |= PTE_R;
	if (prot & 0x2) perm |= PTE_W;
	if (prot & 0x4) perm |= PTE_X;
	
	// Round to page boundaries
	uint64 start_page = PGROUNDDOWN(start);
	uint64 end_page = PGROUNDUP(start + len);
	
	// Map each page
	for (uint64 addr = start_page; addr < end_page; addr += PGSIZE) {
		// Check if already mapped
		if (walkaddr(p->pagetable, addr) != 0) {
			return -1;
		}
		
		// Allocate physical page
		void *pa = kalloc();
		if (pa == 0) {
			return -1;
		}
		
		// Clear the page
		memset(pa, 0, PGSIZE);
		
		// Map the page
		if (mappages(p->pagetable, addr, PGSIZE, (uint64)pa, perm) != 0) {
			kfree(pa);
			return -1;
		}
	}
	
	return 0;
}

uint64 sys_munmap(uint64 start, uint64 len)
{
	if (len == 0) {
		return -1;
	}
	
	struct proc *p = curr_proc();
	
	// Reject if start is not page-aligned
	if (start != PGROUNDDOWN(start)) {
		return -1;
	}
	
	// Reject if length doesn't result in full pages
	if (PGROUNDUP(start + len) != start + len) {
		return -1;
	}
	
	// Round to page boundaries
	uint64 start_page = PGROUNDDOWN(start);
	uint64 end_page = PGROUNDUP(start + len);
	
	// First pass: verify all pages are mapped
	for (uint64 addr = start_page; addr < end_page; addr += PGSIZE) {
		if (walkaddr(p->pagetable, addr) == 0) {
			return -1;
		}
	}
	
	// Second pass: unmap all pages
	for (uint64 addr = start_page; addr < end_page; addr += PGSIZE) {
		uvmunmap(p->pagetable, addr, 1, 1);
	}
	
	return 0;
}

uint64 sys_spawn(uint64 va)
{
	struct proc *p = curr_proc();
	
	// Get the program name from user space
	char name[200];
	if (copyinstr(p->pagetable, name, va, 200) < 0) {
		return -1;  // Invalid name pointer
	}
	
	debugf("sys_spawn: %s\n", name);
	
	// Find the program ID by name
	int id = get_id_by_name(name);
	if (id < 0) {
		return -1;  // Program not found
	}
	
	// Allocate a new process
	struct proc *np = allocproc();
	if (np == NULL) {
		return -1;  // Process pool full
	}
	
	// Load the program into the new process
	if (loader(id, np) < 0) {
		freeproc(np);
		return -1;  // Loading failed
	}
	
	// Set parent
	np->parent = p;
	
	// Make it runnable and add to task queue
	np->state = RUNNABLE;
	add_task(np);
	
	// Return child PID
	return np->pid;
}


uint64 sys_set_priority(long long prio)  // Change int64 to long long
{
	// Validate priority range [2, INT64_MAX]
	if (prio < 2) {
		return -1;
	}
	
	struct proc *p = curr_proc();
	
	// Update priority
	p->priority = prio;
	
	// Recalculate pass value
	p->pass = BIG_STRIDE / prio;
	
	return prio;
}


extern char trap_page[];

void syscall()
{
	struct proc *p = curr_proc();
	struct trapframe *trapframe = p->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	
	// PROJECT 1: Update syscall counter
	if (id < 500) {
		p->syscall_times[id]++;
	}
	
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: //fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	case SYS_task_info:  // PROJECT 1: Add this case
		ret = sys_task_info(args[0]);
		break;
	case SYS_mmap:  // PROJECT 2: Add this case
		ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
		break;
	case SYS_munmap:  // PROJECT 2: Add this case
		ret = sys_munmap(args[0], args[1]);
		break;
	case SYS_setpriority:  // ADD THIS CASE
		ret = sys_set_priority(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
