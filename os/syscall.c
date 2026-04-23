#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h"

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
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
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
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
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_spawn(uint64 va)
{
	struct proc *p = curr_proc();
	char name[200];
	if (copyinstr(p->pagetable, name, va, 200) < 0)
		return -1;
	struct inode *ip = namei(name);
	if (ip == 0)
		return -1;
	struct proc *np = allocproc();
	if (np == NULL)
		return -1;
	init_stdio(np);
	bin_loader(ip, np);
	iput(ip);
	np->parent = p;
	np->state = RUNNABLE;
	//add_task(np); remove
	return np->pid;
}

uint64 sys_set_priority(long long prio)
{
	if (prio < 2) {
		return -1;
	}
	struct proc *p = curr_proc();
	p->priority = prio;
	p->pass = BIG_STRIDE / prio;
	return prio;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

// Gets the status/metadata of an open file via its file descriptor.
// The user program passes a pointer to a Stat struct which we fill in.
int sys_fstat(int fd, uint64 stat)
{
		// Validate the file descriptor is within range.
        if (fd < 0 || fd >= FD_BUFFER_SIZE)
                return -1;
        struct proc *p = curr_proc();
        struct file *f = p->files[fd];

		// File descriptor must be open and must point to an inode-backed file.
        // FD_STDIO (stdin/stdout/stderr) don't have inodes so we reject them.
        if (f == NULL || f->type != FD_INODE)
                return -1;

        // Validate that the user's stat pointer is a legal virtual address.
        // useraddr() translates VA->PA and returns 0 if the address is unmapped.
        if (useraddr(p->pagetable, stat) == 0)
                return -1;
        struct inode *ip = f->ip;

		// Make sure the inode's data has been loaded from disk.
        // ivalid() is a no-op if the inode is already loaded (ip->valid==1).
        ivalid(ip);

		// Fill in the Stat struct with the inode's metadata.
        Stat st;
        st.dev = ip->dev; // which disk device
        st.ino = ip->inum; // inode number (unique file ID on this device)

		// Convert the internal type (T_DIR=1, T_FILE=2) to the user-facing
        // mode flags (DIR=0x040000, FILE=0x100000) that test programs expect.

        st.mode = (ip->type == T_DIR) ? DIR : FILE;
        st.nlink = ip->nlink;
        memset(st.pad, 0, sizeof(st.pad));

		// Copy the filled Stat struct from kernel space into user virtual memory.
        // copyout() handles the VA->PA translation for the destination address.
        copyout(p->pagetable, stat, (char *)&st, sizeof(Stat));
        return 0;
}

// Creates a hard link - a second directory entry pointing to
// the same inode as an existing file. After this call, both
// oldpath and newpath refer to the exact same file data.
// olddirfd, newdirfd, flags are ignored (always AT_FDCWD/0).

int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint64 flags)
{
        struct proc *p = curr_proc();
        char old[MAXPATH], new[MAXPATH];

		// Copy both path strings from user virtual memory into kernel buffers.
        if (copyinstr(p->pagetable, old, oldpath, MAXPATH) < 0)
                return -1;
        if (copyinstr(p->pagetable, new, newpath, MAXPATH) < 0)
                return -1;
	
        // Linking a file to itself (same name) is an error per the spec.
        if (strncmp(old, new, MAXPATH) == 0)
                return -1;
        // Find the old inode
        struct inode *ip = namei(old);
        if (ip == NULL)
                return -1;
        ivalid(ip);
        // Only link regular files, not directories
        if (ip->type == T_DIR) {
                iput(ip);
                return -1;
        }
        // Add new directory entry pointing to same inode
        struct inode *dp = root_dir();
        if (dirlink(dp, new, ip->inum) < 0) {
                iput(ip);
                iput(dp);
                return -1;
        }
        // Increment link count
        ip->nlink++;
        iupdate(ip);
        iput(ip);
        iput(dp);
        return 0;
}

int sys_unlinkat(int dirfd, uint64 name, uint64 flags)
{
        struct proc *p = curr_proc();
        char path[MAXPATH];
        if (copyinstr(p->pagetable, path, name, MAXPATH) < 0)
                return -1;
        // Find the inode
        struct inode *ip = namei(path);
        if (ip == NULL)
                return -1;
        ivalid(ip);
        // Remove directory entry
        struct inode *dp = root_dir();
        if (dirunlink(dp, path) < 0) {
                iput(ip);
                iput(dp);
                return -1;
        }
        // Decrement link count
        ip->nlink--;
        iupdate(ip);
        iput(ip);
        iput(dp);
        return 0;
}

extern char trap_page[];

uint64 sys_task_info(uint64 ti_va)
{
	struct proc *p = curr_proc();
	uint64 ti_pa = useraddr(p->pagetable, ti_va);
	if (ti_pa == 0) {
		return -1;
	}
	TaskInfo *ti = (TaskInfo *)ti_pa;
	ti->status = Running;
	for (int i = 0; i < 500; i++) {
		ti->syscall_times[i] = p->syscall_times[i];
	}
	ti->time = ((get_cycle() - p->start_time) * 1000) / CPU_FREQ;
	return 0;
}

uint64 sys_mmap(uint64 start, uint64 len, int prot, int flags, int fd)
{
	if (len == 0 || len > 1024 * 1024 * 1024) return -1;
	if (start != PGROUNDDOWN(start)) return -1;
	if ((prot & ~0x7) != 0) return -1;
	if ((prot & 0x7) == 0) return -1;
	struct proc *p = curr_proc();
	int perm = PTE_U;
	if (prot & 0x1) perm |= PTE_R;
	if (prot & 0x2) perm |= PTE_W;
	if (prot & 0x4) perm |= PTE_X;
	uint64 start_page = PGROUNDDOWN(start);
	uint64 end_page = PGROUNDUP(start + len);
	for (uint64 addr = start_page; addr < end_page; addr += PGSIZE) {
		if (walkaddr(p->pagetable, addr) != 0) return -1;
		void *pa = kalloc();
		if (pa == 0) return -1;
		memset(pa, 0, PGSIZE);
		if (mappages(p->pagetable, addr, PGSIZE, (uint64)pa, perm) != 0) {
			kfree(pa);
			return -1;
		}
	}
	return 0;
}

uint64 sys_munmap(uint64 start, uint64 len)
{
	if (len == 0) return -1;
	if (start != PGROUNDDOWN(start)) return -1;
	if (PGROUNDUP(start + len) != start + len) return -1;
	struct proc *p = curr_proc();
	uint64 start_page = PGROUNDDOWN(start);
	uint64 end_page = PGROUNDUP(start + len);
	for (uint64 addr = start_page; addr < end_page; addr += PGSIZE) {
		if (walkaddr(p->pagetable, addr) == 0) return -1;
	}
	for (uint64 addr = start_page; addr < end_page; addr += PGSIZE) {
		uvmunmap(p->pagetable, addr, 1, 1);
	}
	return 0;
}

void syscall()
{
	struct proc *p = curr_proc();

	struct trapframe *trapframe = p->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
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
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_fstat:
	    ret = sys_fstat(args[0],args[1]);
		break;
	case SYS_linkat:
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		break;
	case SYS_unlinkat:
	    ret = sys_unlinkat(args[0],args[1],args[2]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
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
	case SYS_setpriority:
		ret = sys_set_priority(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
	// PROJECT 1: Update syscall counter
	if (id < 500) {
		p->syscall_times[id]++;
	}
}
