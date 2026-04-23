#ifndef FILE_H
#define FILE_H

#include "fs.h"
#include "proc.h"
#include "types.h"

#define PIPESIZE (512)
#define FILEPOOLSIZE (NPROC * FD_BUFFER_SIZE)

// in-memory copy of an inode,it can be used to quickly locate file entities on disk
struct inode {
	uint dev; // Device number
	uint inum; // Inode number
	int ref; // Reference count
	int valid; // inode has been read from disk?
	short type; // copy of disk inode
	uint size;
	uint addrs[NDIRECT + 1];
	// LAB4: You may need to add link count here
	// PROJECT 4: In-memory copy of the hard link count from the on-disk dinode.
	// Loaded by ivalid() when the inode is first read from disk.
	// Modified by sys_linkat (++) and sys_unlinkat (--).
	// Flushed back to disk by iupdate().
	// When nlink reaches 0 and ref reaches 1, iput() deletes the file data.
	uint nlink; // number of hard links
};

// Defines a file in memory that provides information about the current use of the 
// file and the corresponding inode location
struct file {
	enum { FD_NONE = 0, FD_INODE, FD_STDIO } type;
	int ref; // reference count
	char readable;
	char writable;
	struct inode *ip; // FD_INODE
	uint off;
};

//A few specific fd
enum {
	STDIN = 0,
	STDOUT = 1,
	STDERR = 2,
};

extern struct file filepool[FILEPOOLSIZE];

void fileclose(struct file *);
struct file *filealloc();
int fileopen(char *, uint64);
uint64 inodewrite(struct file *, uint64, uint64);
uint64 inoderead(struct file *, uint64, uint64);
struct file *stdio_init(int);
int show_all_files();

#endif // FILE_H