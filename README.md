# x86 32 Bit EstrOS 

Loads a x86 32 bit app into memory address 0x300000 (3Mb) and provides some basic syscalls.

## Making an App

The app should be put into the ./build/root directory as a linked elf file for 0x400000.

## Syscalls

All syscalls are on interrupt 0x40 (decimal 64).
The value in `eax` will specify the operation (see table below).

### 0x00
```
nop
```

### 0x01
```
nop
```

### 0x02 open
```
input
ebx = path to file (char*)
ecx = flags
output
ebx = file pointer
```

### 0x03 close
```
input
ebx = file pointer
```

### 0x04 read
```
input
ebx = file pointer
ecx = size
edx = buffer
output
eax = number of bytes read
```

### 0x05 write
```
input
ebx = file pointer
ecx = size
edx = buffer
output
eax = number of bytes written
```

### 0x06 ioctl
```
input
ebx = file pointer
ecx = command
edx = arg
output (specified by driver)
ecx
edx
```

### 0x07 seek
```
input
ebx = file pointer
ecx = offset
edx = whence
```

### 0x08 tell
```
input
ebx = file pointer
ouput
ecx = position
```

### 0x09 create file
```
input
ebx = file path (char*)
output
eax = return value (0 for success)
```

### 0x0a - 0x0f nop

### 0x10 request new page
```
input
ebx = new page physical address (set to pager error (-1)) for any free page
edx = page table (set to  pager error (-1) for it to use the current apps page table)
output
ebx = virtual address of the new page (returns pager error (-1) on fail)
```

### 0x11 free page
```
input
ebx = page address
edx = page table (set to  pager error (-1) for it to use the current apps page table)
```

### 0x12 create new table
```
output
ebx = pointer to the new page table
```

### 0x13 - 0x1f nop

### 0x20 get current process (get self)
```
output
ebx = pointer to the process struct
```

### 0x21 create process
```
input
ebx = pointer to process init struct
output
ebx = pointer to process struct
```

### 0x22 exit
```
input
ebx = exit code
```


## File System
EstrOS File System v1
(This definitely is not mostly copied from ext2)

Block Size = 1024 bytes

File Types:
File = 1
Directory = 2

Bit Maps:
0 = free
1 = used

General Structure
offset       | length                                   | description
-------------|------------------------------------------|-------------
 0x10000     | 0x0c                                     | Super Block
 +block_size | 0xffffffff / block_size / 8              | free block bit map
 +last length| max_inodes * inode_size / block_size / 8 | free inode bit map
 +last length| inodes_max * inode_size / block_size     | Index Nodes
 +last length| ....                                     | Data Blocks

Super Block @ 0x10000 on the drive
offset | length | description
-------|--------|-------------
0x00   | 0x08   | Number of bytes in the filesystem
0x08   | 0x04   | Number of blocks in the filesystem
0x0c   | 0x04   | Max number of index nodes in the filesystem
0x10   | 0x04   | Number of inodes in the filesystem
0x14   | 0x04   | root index node number

Index Node (64 bytes)
offset | length | description
-------|--------|-------------
0x00   | 0x04   | type
0x04   | 0x04   | size
0x08   | 0x04   | block0
0x0c   | 0x04   | block1
0x10   | 0x04   | block2
0x14   | 0x04   | block3
0x18   | 0x04   | block4
0x1c   | 0x04   | block5
0x20   | 0x04   | block6
0x24   | 0x04   | block7
0x28   | 0x04   | block8
0x2c   | 0x04   | block9
0x30   | 0x04   | block10 (single indirect)
0x34   | 0x04   | block11 (double indirect)
0x38   | 0x04   | reserved 
0x3c   | 0x04   | reserved

Directory Entry
offset | length | description
-------|--------|-------------
0x00   | 0x04   | index node number
0x04   | 0x04   | entry length
0x08   | 0x04   | name length
0x0c   | ....   | name[] (not null terminated)

