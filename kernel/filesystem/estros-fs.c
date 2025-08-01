#include "estros-fs.h"
#include <filesystem/virtual-filesystem.h>
#include <harddrive/hdd.h>
#include <heap.h>
#include <memutils.h>
#include <stdint.h>

enum BitMap {
    FREE = 0,
    USED = 1,
};

enum InodeType {
    FILE = 1,
    DIRECTORY = 2
};

struct Inode {
    uint32_t type;
    uint32_t size;
    uint32_t blocks[13];
    uint32_t reserved;
};

struct InodeData {
    uint32_t inode_number;
    uint32_t blocks[13];
};

struct DirectoryEntry {
    uint32_t inode_number;
    uint32_t entry_length;
    uint32_t name_length;
    char name[];
};

struct SuperBlock {
    uint64_t size_bytes;
    uint32_t n_blocks;
    uint32_t max_inodes;
    uint32_t n_inodes;
    uint32_t root_inode;
};

struct FileData {
    uint8_t* data;
    uint32_t range_low;
    uint32_t range_high;
};

struct SuperBlock super_block = { 0 };

#define SINGLE_INDIRECT_INDEX NUM_DIRECT_BLOCKS
#define DOUBLE_INDIRECT_INDEX (NUM_DIRECT_BLOCKS + 1)
#define TRIPLE_INDIRECT_INDEX (NUM_DIRECT_BLOCKS + 2)
#define PTRS_PER_BLOCK (BLOCK_SIZE / sizeof(uint32_t))
#define NUM_DIRECT_BLOCKS 10
#define BLOCK_SIZE 1024
#define INODE_SIZE 64
#define MAX_INODES 8192
#define INODE_SPACE (MAX_INODES * INODE_SIZE)
#define SUPER_BLOCK_OFFSET 0x10000
#define BLOCK_BP_SIZE (0xffffffff / BLOCK_SIZE / 8)
#define INODE_BP_SIZE (MAX_INODES * INODE_SIZE / BLOCK_SIZE / 8)
#define FIRST_DATA_BLOCK (BLOCK_SIZE + BLOCK_BP_SIZE + INODE_BP_SIZE + INODE_SIZE) / BLOCK_SIZE + 1)

VFSFile* harddrive = NULL;

void fs_set_harddrive(char* path)
{
    harddrive = vfs_open_file(path, 0);
    uint8_t sector[512];
    vfs_seek(harddrive, 0x10000, VFS_BEG);
    vfs_read(harddrive, sector, 512);
    memcpy(&super_block, sector, sizeof(struct SuperBlock));
    return;
};

uint32_t alloc_block()
{
    uint32_t block = super_block.n_blocks;
    super_block.n_blocks++;
    uint8_t sector[512];
    memcpy(sector, &super_block, sizeof(struct SuperBlock));
    vfs_seek(harddrive, 0x10000, VFS_BEG);
    vfs_write(harddrive, sector, 512);
    return block;
}

uint32_t alloc_inode()
{
    uint32_t block = super_block.n_inodes;
    super_block.n_inodes++;
    uint8_t sector[512];
    memcpy(sector, &super_block, sizeof(struct SuperBlock));
    vfs_seek(harddrive, 0x10000, VFS_BEG);
    vfs_write(harddrive, sector, 512);
    return block; // Out of space
}

VFSIndexNode fstovfs(struct Inode inode, uint32_t inode_number)
{
    VFSIndexNode vfs_inode = {
        .type = inode.type,
        .size = inode.size,
        .private_data = (uint32_t*)malloc(sizeof(struct InodeData)),
        .file_operations = get_fs_file_operations(),
        .number_of_references = 0,
    };

    if (vfs_inode.private_data == NULL) {
        vfs_inode.type = VFS_ERROR;
        return vfs_inode;
    }

    struct InodeData* id = vfs_inode.private_data;
    memcpy(id->blocks, inode.blocks, sizeof(uint32_t) * 13);
    id->inode_number = inode_number;
    return vfs_inode;
}

struct Inode vfstofs(VFSIndexNode* vfs_inode)
{
    struct Inode inode = {
        .type = vfs_inode->type,
        .size = vfs_inode->size,
    };
    memcpy(inode.blocks, ((struct InodeData*)vfs_inode->private_data)->blocks, sizeof(uint32_t) * 13);
    return inode;
}

void write_inode(struct Inode inode, uint32_t inode_number)
{
    uint32_t inode_offset = ((SUPER_BLOCK_OFFSET + BLOCK_SIZE + BLOCK_BP_SIZE + INODE_BP_SIZE) / BLOCK_SIZE + 1) * BLOCK_SIZE
        + (inode_number * INODE_SIZE);

    uint8_t buffer[512];

    vfs_seek(harddrive, inode_offset, VFS_BEG);
    vfs_read(harddrive, buffer, 512);

    uint32_t sub_offset = inode_number % 8;
    memcpy(&buffer[sub_offset * INODE_SIZE], &inode, INODE_SIZE);

    vfs_seek(harddrive, inode_offset, VFS_BEG);
    vfs_write(harddrive, buffer, 512);
}

void* harddrive_load_blocks(void* buffer, uint32_t blocks[13], uint32_t pos, uint32_t num_blocks)
{
    for (uint32_t i = 0; i < num_blocks; i++) {

        uint32_t block_n = (pos + i * BLOCK_SIZE) / BLOCK_SIZE;

        uint32_t real_block = 0xFFFFFFFF;

        if (block_n < NUM_DIRECT_BLOCKS) {
            real_block = blocks[block_n];
        } else if (block_n < NUM_DIRECT_BLOCKS + PTRS_PER_BLOCK) {
            // Single indirect
            uint32_t index = block_n - NUM_DIRECT_BLOCKS;
            uint32_t single_ptrs[PTRS_PER_BLOCK];
            vfs_seek(harddrive, blocks[SINGLE_INDIRECT_INDEX] * BLOCK_SIZE, VFS_BEG);
            vfs_read(harddrive, single_ptrs, BLOCK_SIZE);
            real_block = single_ptrs[index];
        } else if (block_n < NUM_DIRECT_BLOCKS + PTRS_PER_BLOCK + PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
            // Double indirect
            uint32_t index = block_n - NUM_DIRECT_BLOCKS - PTRS_PER_BLOCK;
            uint32_t l1 = index / PTRS_PER_BLOCK;
            uint32_t l2 = index % PTRS_PER_BLOCK;

            uint32_t double_ptrs[PTRS_PER_BLOCK];
            uint32_t single_ptrs[PTRS_PER_BLOCK];

            vfs_seek(harddrive, blocks[DOUBLE_INDIRECT_INDEX] * BLOCK_SIZE, VFS_BEG);
            vfs_read(harddrive, double_ptrs, BLOCK_SIZE);

            vfs_seek(harddrive, double_ptrs[l1] * BLOCK_SIZE, VFS_BEG);
            vfs_read(harddrive, single_ptrs, BLOCK_SIZE);

            real_block = single_ptrs[l2];
        } else {
            // Triple indirect
            uint32_t index = block_n - NUM_DIRECT_BLOCKS - PTRS_PER_BLOCK - PTRS_PER_BLOCK * PTRS_PER_BLOCK;
            uint32_t l1 = index / (PTRS_PER_BLOCK * PTRS_PER_BLOCK);
            uint32_t l2 = (index / PTRS_PER_BLOCK) % PTRS_PER_BLOCK;
            uint32_t l3 = index % PTRS_PER_BLOCK;

            uint32_t triple_ptrs[PTRS_PER_BLOCK];
            uint32_t double_ptrs[PTRS_PER_BLOCK];
            uint32_t single_ptrs[PTRS_PER_BLOCK];

            vfs_seek(harddrive, blocks[TRIPLE_INDIRECT_INDEX] * BLOCK_SIZE, VFS_BEG);
            vfs_read(harddrive, triple_ptrs, BLOCK_SIZE);

            vfs_seek(harddrive, triple_ptrs[l1] * BLOCK_SIZE, VFS_BEG);
            vfs_read(harddrive, double_ptrs, BLOCK_SIZE);

            vfs_seek(harddrive, double_ptrs[l2] * BLOCK_SIZE, VFS_BEG);
            vfs_read(harddrive, single_ptrs, BLOCK_SIZE);

            real_block = single_ptrs[l3];
        }

        if (real_block == 0 || real_block == 0xFFFFFFFF) {
            if (i != 0) {
                return buffer;
            }
            return NULL;
        }

        vfs_seek(harddrive, real_block * BLOCK_SIZE, VFS_BEG);
        vfs_read(harddrive, buffer + (i * BLOCK_SIZE), BLOCK_SIZE);
    }

    return buffer;
}

void harddrive_write_blocks(void* buffer, uint32_t blocks[13], uint32_t pos, uint32_t num_blocks)
{
    for (uint32_t i = 0; i < num_blocks; i++) {
        uint32_t block_n = (pos + i * BLOCK_SIZE) / BLOCK_SIZE;
        uint32_t real_block = 0xFFFFFFFF;

        // --- Direct blocks ---
        if (block_n < NUM_DIRECT_BLOCKS) {
            if (blocks[block_n] == 0) {
                blocks[block_n] = alloc_block();
            }
            real_block = blocks[block_n];
        }

        // --- Single Indirect ---
        else if (block_n < NUM_DIRECT_BLOCKS + PTRS_PER_BLOCK) {
            uint32_t index = block_n - NUM_DIRECT_BLOCKS;
            uint32_t single_ptrs[PTRS_PER_BLOCK] = { 0 };

            if (blocks[SINGLE_INDIRECT_INDEX] == 0) {
                blocks[SINGLE_INDIRECT_INDEX] = alloc_block();
            }

            vfs_seek(harddrive, blocks[SINGLE_INDIRECT_INDEX] * BLOCK_SIZE, VFS_BEG);
            vfs_read(harddrive, single_ptrs, BLOCK_SIZE);

            if (single_ptrs[index] == 0) {
                single_ptrs[index] = alloc_block();
                vfs_seek(harddrive, blocks[SINGLE_INDIRECT_INDEX] * BLOCK_SIZE, VFS_BEG);
                vfs_write(harddrive, single_ptrs, BLOCK_SIZE);
            }

            real_block = single_ptrs[index];
        }

        // --- Double Indirect ---
        else if (block_n < NUM_DIRECT_BLOCKS + PTRS_PER_BLOCK + PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
            uint32_t index = block_n - NUM_DIRECT_BLOCKS - PTRS_PER_BLOCK;
            uint32_t l1 = index / PTRS_PER_BLOCK;
            uint32_t l2 = index % PTRS_PER_BLOCK;

            uint32_t double_ptrs[PTRS_PER_BLOCK] = { 0 };
            uint32_t single_ptrs[PTRS_PER_BLOCK] = { 0 };

            if (blocks[DOUBLE_INDIRECT_INDEX] == 0) {
                blocks[DOUBLE_INDIRECT_INDEX] = alloc_block();
            }

            vfs_seek(harddrive, blocks[DOUBLE_INDIRECT_INDEX] * BLOCK_SIZE, VFS_BEG);
            vfs_read(harddrive, double_ptrs, BLOCK_SIZE);

            if (double_ptrs[l1] == 0) {
                double_ptrs[l1] = alloc_block();
                vfs_seek(harddrive, blocks[DOUBLE_INDIRECT_INDEX] * BLOCK_SIZE, VFS_BEG);
                vfs_write(harddrive, double_ptrs, BLOCK_SIZE);
            }

            vfs_seek(harddrive, double_ptrs[l1] * BLOCK_SIZE, VFS_BEG);
            vfs_read(harddrive, single_ptrs, BLOCK_SIZE);

            if (single_ptrs[l2] == 0) {
                single_ptrs[l2] = alloc_block();
                vfs_seek(harddrive, double_ptrs[l1] * BLOCK_SIZE, VFS_BEG);
                vfs_write(harddrive, single_ptrs, BLOCK_SIZE);
            }

            real_block = single_ptrs[l2];
        }

        // --- Triple Indirect ---
        else {
            // uint32_t index = block_n - NUM_DIRECT_BLOCKS - PTRS_PER_BLOCK - PTRS_PER_BLOCK * PTRS_PER_BLOCK;
            // uint32_t l1 = index / (PTRS_PER_BLOCK * PTRS_PER_BLOCK);
            // uint32_t l2 = (index / PTRS_PER_BLOCK) % PTRS_PER_BLOCK;
            // uint32_t l3 = index % PTRS_PER_BLOCK;
            //
            // uint32_t triple_ptrs[PTRS_PER_BLOCK] = {0};
            // uint32_t double_ptrs[PTRS_PER_BLOCK] = {0};
            // uint32_t single_ptrs[PTRS_PER_BLOCK] = {0};
            //
            // if (blocks[TRIPLE_INDIRECT_INDEX] == 0) {
            //     blocks[TRIPLE_INDIRECT_INDEX] = alloc_block();
            // }
            //
            // vfs_seek(harddrive, blocks[TRIPLE_INDIRECT_INDEX] * BLOCK_SIZE, VFS_BEG);
            // vfs_read(harddrive, triple_ptrs, BLOCK_SIZE);
            //
            // if (triple_ptrs[l1] == 0) {
            //     triple_ptrs[l1] = alloc_block();
            //     vfs_seek(harddrive, blocks[TRIPLE_INDIRECT_INDEX] * BLOCK_SIZE, VFS_BEG);
            //     vfs_write(harddrive, triple_ptrs, BLOCK_SIZE);
            // }
            //
            // vfs_seek(harddrive, triple_ptrs[l1] * BLOCK_SIZE, VFS_BEG);
            // vfs_read(harddrive, double_ptrs, BLOCK_SIZE);
            //
            // if (double_ptrs[l2] == 0) {
            //     double_ptrs[l2] = alloc_block();
            //     vfs_seek(harddrive, triple_ptrs[l1] * BLOCK_SIZE, VFS_BEG);
            //     vfs_write(harddrive, double_ptrs, BLOCK_SIZE);
            // }
            //
            // vfs_seek(harddrive, double_ptrs[l2] * BLOCK_SIZE, VFS_BEG);
            // vfs_read(harddrive, single_ptrs, BLOCK_SIZE);
            //
            // if (single_ptrs[l3] == 0) {
            //     single_ptrs[l3] = alloc_block();
            //     vfs_seek(harddrive, double_ptrs[l2] * BLOCK_SIZE, VFS_BEG);
            //     vfs_write(harddrive, single_ptrs, BLOCK_SIZE);
            // }
            //
            // real_block = single_ptrs[l3];
        }

        if (real_block == 0 || real_block == 0xFFFFFFFF) {
            return; // out of space
        }

        vfs_seek(harddrive, real_block * BLOCK_SIZE, VFS_BEG);
        vfs_write(harddrive, (uint8_t*)buffer + (i * BLOCK_SIZE), BLOCK_SIZE);
    }
}

#define BUFFER_SIZE_BLOCKS 5

VFSFile* fs_open(VFSIndexNode* inode)
{
    VFSFile* file = (VFSFile*)malloc(sizeof(VFSFile));
    if (file == NULL) {
        return NULL;
    }
    file->inode = inode;
    file->private_data = malloc(sizeof(struct FileData));
    file->private_data_size = 0;
    file->position = 0;
    if (file->private_data == NULL) {
        free(file);
        return NULL;
    }
    struct FileData* fd = file->private_data;
    fd->range_low = 0;
    fd->range_high = 0;
    fd->data = malloc(BLOCK_SIZE * BUFFER_SIZE_BLOCKS);
    if (fd->data == NULL) {
        free(file->private_data);
        free(file);
        return NULL;
    }
    return file;
}

void fs_close(VFSFile* file)
{
    if (file->private_data != NULL) {
        free(file->private_data);
    }
    free(file);
}

uint32_t fs_read(VFSFile* file, void* buffer, uint32_t buffer_size)
{
    if (buffer_size == 0) {
        return 0;
    }
    if (file->position >= file->inode->size) {
        return 0;
    }
    if (buffer_size + file->position > file->inode->size) {
        buffer_size = file->inode->size - file->position;
    }

    struct FileData* fd = file->private_data;
    struct InodeData* id = file->inode->private_data;

    uint32_t remaining_size = buffer_size;
    uint32_t bytes_read = 0;
    while (remaining_size > 0) {
        if (remaining_size > BUFFER_SIZE_BLOCKS * BLOCK_SIZE) {
            buffer_size = BUFFER_SIZE_BLOCKS * BLOCK_SIZE;
        }
        remaining_size -= buffer_size;

        if (file->position + buffer_size > fd->range_high || file->position < fd->range_low || fd->range_high == fd->range_low) {
            void* temp = harddrive_load_blocks(fd->data, id->blocks, file->position, BUFFER_SIZE_BLOCKS);
            if (temp == NULL) {
                return 0;
            }
            fd->data = temp;

            fd->range_low = (file->position / BLOCK_SIZE) * BLOCK_SIZE;
            fd->range_high = fd->range_low + (BUFFER_SIZE_BLOCKS * BLOCK_SIZE);
        }

        uint32_t i;
        for (i = 0; i < buffer_size; i++) {
            ((uint8_t*)buffer)[i] = fd->data[i + file->position - fd->range_low];
        }
        file->position += i;
        bytes_read += i;
    }
    return bytes_read;
}

uint32_t fs_write(VFSFile* file, void* buffer, uint32_t buffer_size)
{
    struct FileData* fd = file->private_data;
    struct InodeData* id = file->inode->private_data;
    if (file->position + buffer_size > fd->range_high || file->position < fd->range_low || fd->range_high == fd->range_low) {
        if (file->inode->size == 0) {

            fd->data = malloc(BLOCK_SIZE * 5);

        } else {

            fd->data = harddrive_load_blocks(fd->data, id->blocks, file->position, 5);
            if (fd->data == NULL) {
                return 0;
            }
        }

        fd->range_low = (file->position / BLOCK_SIZE) * BLOCK_SIZE;
        fd->range_high = fd->range_low + (5 * BLOCK_SIZE);
    }

    uint32_t i;
    for (i = 0; i < buffer_size; i++) {
        fd->data[i + file->position - fd->range_low] = ((uint8_t*)buffer)[i];
    }
    file->position += i;
    if (file->position > file->inode->size) {
        file->inode->size = file->position;
    }
    uint32_t number_to_write = 5;
    if (number_to_write > (file->inode->size / BLOCK_SIZE) + 1) {
        number_to_write = (file->inode->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    }
    harddrive_write_blocks(fd->data, id->blocks, fd->range_low, number_to_write);

    write_inode(vfstofs(file->inode), id->inode_number);

    return i;
}

void fs_ioctl(VFSFile* file, uint32_t* command, uint32_t* arg) { }
void fs_seek(VFSFile* file, uint32_t offset, uint32_t whence)
{
    switch (whence) {
    case VFS_BEG:
        if (offset < file->inode->size - 1) {
            file->position = offset;
        } else {
            file->position = file->inode->size - 1;
        }
        break;
    case VFS_CUR:
        if (offset + file->position < file->inode->size - 1) {
            file->position += offset;
        } else {
            file->position += file->inode->size - 1;
        }
        break;
    case VFS_END:
        if (file->inode->size - 1 - offset > 0) {
            file->position = file->inode->size - offset;
        } else {
            file->position = 0;
        }
        break;
    }
}
uint32_t fs_tell(VFSFile* file)
{
    return file->position;
}
void fs_flush(VFSFile* file) { }

VFSFileOperations get_fs_file_operations()
{
    VFSFileOperations fops = {
        .open = (void*)fs_open,
        .close = (void*)fs_close,
        .read = (void*)fs_read,
        .write = (void*)fs_write,
        .ioctl = (void*)fs_ioctl,
        .seek = (void*)fs_seek,
        .tell = (void*)fs_tell,
        .flush = (void*)fs_flush,
    };
    return fops;
}

struct Inode fetch_inode(uint32_t inode_number)
{
    uint32_t inode_offset = ((SUPER_BLOCK_OFFSET + BLOCK_SIZE + BLOCK_BP_SIZE + INODE_BP_SIZE) / BLOCK_SIZE + 1) * BLOCK_SIZE
        + (inode_number * INODE_SIZE);

    uint8_t buffer[512];
    vfs_seek(harddrive, inode_offset, VFS_BEG);
    vfs_read(harddrive, buffer, 512);
    uint32_t sub_offset = inode_number % 8;
    struct Inode inode;
    memcpy(&inode, &buffer[sub_offset * INODE_SIZE], INODE_SIZE);
    return inode;
}

struct DirectoryEntry* fetch_inode_directory(struct Inode inode)
{
    if (inode.size == 0) {
        return NULL;
    }
    struct DirectoryEntry* entries = (struct DirectoryEntry*)malloc((inode.size / 512 + 1) * 512);
    if (entries == NULL) {
        return NULL;
    }
    memset(entries, 0, (inode.size / 512 + 1) * 512);
    vfs_seek(harddrive, inode.blocks[0] * BLOCK_SIZE, VFS_BEG);
    vfs_read(harddrive, entries, (inode.size / 512 + 1) * 512);

    return entries;
}

struct Inode resolve_inode(char* path)
{
    struct Inode err = { 0 };
    while (*path == '/') {
        path++;
    }
    struct Inode inode = fetch_inode(super_block.root_inode);
    if (strlen(path) == 0) {
        return inode;
    }
    while (1) {
        struct DirectoryEntry* base_entry = fetch_inode_directory(inode);
        if (base_entry == NULL) {
            return err;
        }
        struct DirectoryEntry* entry = base_entry;

        uint32_t offset = 0;
        for (int i = 0; i < strlen(path); i++) {
            if (path[i] == '/') {
                offset = i;
                if (offset != 0) {
                    path[i] = '\0';
                }
                break;
            }
        }

        if (offset != 0) {
            if (inode.type != DIRECTORY) {
                free(base_entry);
                return err;
            }
        }

        while (strncmp(path, entry->name, entry->name_length) != 0 || strlen(path) > entry->name_length) {
            if (entry->entry_length == 0) {
                free(base_entry);
                return err;
            }
            entry = (struct DirectoryEntry*)((uint8_t*)entry + entry->entry_length);
        }
        inode = fetch_inode(entry->inode_number);
        if (offset != 0) {
            path[offset] = '/';
        }
        path += offset + 1;

        if (offset == 0) { // found
            free(base_entry);
            break;
        }

        free(base_entry);
    }
    return inode;
}

uint32_t resolve_inode_number(char* path)
{
    while (*path == '/') {
        path++;
    }
    uint32_t inode_number = super_block.root_inode;
    struct Inode inode = fetch_inode(super_block.root_inode);
    if (strlen(path) == 0) {
        return inode_number;
    }
    while (1) {
        struct DirectoryEntry* base_entry = fetch_inode_directory(inode);
        if (base_entry == NULL) {
            return -1;
        }
        struct DirectoryEntry* entry = base_entry;

        uint32_t offset = 0;
        for (int i = 0; i < strlen(path); i++) {
            if (path[i] == '/') {
                offset = i;
                if (offset != 0) {
                    path[i] = '\0';
                }
                break;
            }
        }

        if (offset != 0) {
            if (inode.type != DIRECTORY) {
                free(base_entry);
                return -1;
            }
        }

        while (strncmp(path, entry->name, entry->name_length) != 0 || strlen(path) > entry->name_length) {
            if (entry->entry_length == 0) {
                free(base_entry);
                return -1;
            }
            entry = (struct DirectoryEntry*)((uint8_t*)entry + entry->entry_length);
        }
        inode_number = entry->inode_number;
        inode = fetch_inode(entry->inode_number);
        if (offset != 0) {
            path[offset] = '/';
        }
        path += offset + 1;

        if (offset == 0) { // found
            free(base_entry);
            break;
        }

        free(base_entry);
    }
    return inode_number;
}

int create_inode(char* path, char* name, VFSFileType type)
{
    struct Inode inode = resolve_inode(path);

    if (inode.type != DIRECTORY) {
        return 1;
    }

    char* full_path = malloc(strlen(path) + 1 + strlen(name) + 1);

    if (strlen(path) <= 1) {
        full_path[strlen(path) + strlen(name)] = '\0';
        strncpy(full_path, path, strlen(path));
        strncpy(full_path + strlen(path), name, strlen(name));
    } else {
        full_path[strlen(path) + 1 + strlen(name)] = '\0';
        strncpy(full_path, path, strlen(path));
        full_path[strlen(path)] = '/';
        strncpy(full_path + strlen(path) + 1, name, strlen(name));
    }

    if (resolve_inode(full_path).type != 0) {
        return 1; // already exists
    }

    free(full_path);

    struct DirectoryEntry* new_entry = malloc(sizeof(struct DirectoryEntry) + strlen(name));

    new_entry->inode_number = alloc_inode();
    new_entry->entry_length = sizeof(struct DirectoryEntry) + strlen(name);
    new_entry->name_length = strlen(name);
    strncpy(new_entry->name, name, strlen(name));

    uint8_t dir_buffer[BLOCK_SIZE * 2];
    memset(dir_buffer, 0, BLOCK_SIZE * 2);

    if (inode.size > 0) {
        void* temp = harddrive_load_blocks(dir_buffer, inode.blocks, inode.size, 2);
        if (temp == NULL) {
            free(new_entry);
            return 1;
        }
    }

    struct DirectoryEntry* dentry = (void*)dir_buffer;

    dentry = (struct DirectoryEntry*)((uint32_t)dentry + inode.size);

    memcpy(dentry, new_entry, new_entry->entry_length);

    harddrive_write_blocks(dir_buffer, inode.blocks, inode.size, 2);

    inode.size += new_entry->entry_length;
    write_inode(inode, resolve_inode_number(path));

    struct Inode new_inode = {
        .type = type,
        .size = 0,
        .blocks = { 0 },
    };

    write_inode(new_inode, new_entry->inode_number);

    free(new_entry);

    return 0;
}

VFSIndexNode get_inode(char* path)
{
    struct Inode inode = resolve_inode(path);
    if (inode.type == 0) {
        VFSIndexNode vfs_inode = { 0 };
        vfs_inode.type = VFS_ERROR;
        return vfs_inode;
    }

    uint32_t inode_number = resolve_inode_number(path);
    VFSIndexNode vfs_inode = fstovfs(inode, inode_number);

    return vfs_inode;
}

VFSDirectory* get_directory(char* path, VFSIndexNode* inode)
{
    if (inode->type != VFS_DIRECTORY) {
        return NULL;
    }

    VFSDirectory* vfs_dir = (VFSDirectory*)malloc(sizeof(VFSDirectory));
    if (vfs_dir == NULL) {
        return NULL;
    }
    vfs_dir->entries_length = 0;
    vfs_dir->entries = NULL;
    vfs_dir->inode = inode;

    struct DirectoryEntry* base_entry = fetch_inode_directory(vfstofs(inode));
    if (base_entry == NULL) {
        return vfs_dir;
    }
    struct DirectoryEntry* entry = base_entry;

    vfs_dir->entries = (VFSDirectoryEntry*)malloc(sizeof(VFSDirectoryEntry));
    if (vfs_dir->entries == NULL) {
        free(vfs_dir);
        free(base_entry);
        return NULL;
    }

    while (entry->entry_length != 0) {
        VFSDirectoryEntry vfs_entry;
        if (strlen(path) == 1) {
            vfs_entry.path = (char*)malloc(strlen(path) + entry->name_length + 1);
            strncpy(vfs_entry.path, path, strlen(path));
            strncpy(vfs_entry.path + strlen(path), entry->name, entry->name_length);
            vfs_entry.path[strlen(path) + entry->name_length] = '\0';
        } else {
            vfs_entry.path = (char*)malloc(strlen(path) + 1 + entry->name_length + 1);
            strncpy(vfs_entry.path, path, strlen(path));
            vfs_entry.path[strlen(path)] = '/';
            strncpy(vfs_entry.path + strlen(path) + 1, entry->name, entry->name_length);
            vfs_entry.path[strlen(path) + entry->name_length + 1] = '\0';
        }

        void* temp = (VFSDirectoryEntry*)realloc(vfs_dir->entries, sizeof(VFSDirectoryEntry) * (vfs_dir->entries_length + 1));
        if (temp == NULL) {
            free(temp);
            free(vfs_entry.path);
            free(vfs_dir->entries);
            free(vfs_dir);
            return NULL;
        }
        vfs_dir->entries = temp;
        vfs_dir->entries[vfs_dir->entries_length] = vfs_entry;
        vfs_dir->entries_length++;
        entry = (struct DirectoryEntry*)((uint8_t*)entry + entry->entry_length);
    }
    free(base_entry);
    return vfs_dir;
}

void free_inode_data(VFSIndexNode inode)
{
    free(inode.private_data);
    return;
}

VFSDriverOperations get_fs_driver_operations()
{
    VFSDriverOperations dops = {
        .create_inode = create_inode,
        .get_inode = get_inode,
        .get_directory = get_directory,
        .free_inode_data = free_inode_data,
    };
    return dops;
}
