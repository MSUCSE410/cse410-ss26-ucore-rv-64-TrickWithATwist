#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
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

uint64 sys_gettimeofday(uint64 val_va, int _tz) // TODO: implement sys_gettimeofday in pagetable. (VA to PA)
{
	// YOUR CODE
	struct proc *p = curr_proc();
	
	// Translate virtual address to physical address
	uint64 val_pa = useraddr(p->pagetable, val_va);
	if (val_pa == 0) {
		return -1;  // Translation failed - invalid address
	}
	
	// Now we can safely access the physical memory
	TimeVal *val = (TimeVal *)val_pa;
	
	// Fill in the time
	uint64 cycle = get_cycle();
	val->sec = cycle / CPU_FREQ;
	val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	
	return 0;
}

// TODO: add support for mmap and munmap syscall.
// hint: read through docstrings in vm.c. Watching CH4 video may also help.
// Note the return value and PTE flags (especially U,X,W,R)
uint64 sys_mmap(uint64 start, uint64 len, int prot, int flags, int fd)
{
	// Validate parameters
	if (len == 0 || len > 1024 * 1024 * 1024) {  // Max 1GB
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
	int perm = PTE_U;  // User accessible
	if (prot & 0x1) perm |= PTE_R;  // Readable
	if (prot & 0x2) perm |= PTE_W;  // Writable
	if (prot & 0x4) perm |= PTE_X;  // Executable
	
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
	
	return 0;  // Success
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
	
	// Round to page boundaries (should already be aligned now)
	uint64 start_page = PGROUNDDOWN(start);
	uint64 end_page = PGROUNDUP(start + len);
	
	// First pass: verify all pages are mapped
	for (uint64 addr = start_page; addr < end_page; addr += PGSIZE) {
		if (walkaddr(p->pagetable, addr) == 0) {
			return -1;  // Unmapped page found
		}
	}
	
	// Second pass: unmap all pages
	for (uint64 addr = start_page; addr < end_page; addr += PGSIZE) {
		uvmunmap(p->pagetable, addr, 1, 1);
	}
	
	return 0;
}


/*
* LAB1: you may need to define sys_task_info here
*/
uint64 sys_task_info(uint64 ti_va)
{
	struct proc *p = curr_proc();
	
	// Translate virtual address to physical address
	uint64 ti_pa = useraddr(p->pagetable, ti_va);
	if (ti_pa == 0) {
		return -1;  // Translation failed - invalid address
	}
	
	// Now we can safely access the physical memory
	TaskInfo *ti = (TaskInfo *)ti_pa;
	
	// Fill in the status
	ti->status = Running;
	
	// Copy the syscall counts from the process to the TaskInfo structure
	for (int i = 0; i < 500; i++) {
		ti->syscall_times[i] = p->syscall_times[i];
	}
	
	// Calculate runtime: time since process started (not since boot!)
	ti->time = ((get_cycle() - p->start_time) * 1000) / CPU_FREQ;
	
	return 0;
}

extern char trap_page[];

void syscall()
{
	struct proc *p = curr_proc();
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	/*
	* LAB1: you may need to update syscall counter for task info here
	*/
	if (id < 500) {
		p->syscall_times[id]++;
	}	


	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_task_info:
		ret = sys_task_info(args[0]);
		break;
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
		break;
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = p->pid;
		break;
	/*
	* LAB1: you may need to add SYS_taskinfo case here
	*/
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
