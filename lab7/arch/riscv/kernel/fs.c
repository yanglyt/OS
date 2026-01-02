#include "fs.h"
#include "buf.h"
#include "defs.h"
#include "mm.h"
#include "slub.h"
#include "task_manager.h"
#include "virtio.h"
#include "vm.h"


// --------------------------------------------------
// ----------- read and write interface -------------

/*基础的磁盘读写接口*/
void disk_op(int blockno, uint8_t *data, bool write) {
    struct buf b;
    b.disk = 0;
    b.blockno = blockno;
    b.data = (uint8_t *)PHYSICAL_ADDR(data);
    virtio_disk_rw((struct buf *)(PHYSICAL_ADDR(&b)), write);
}

#define disk_read(blockno, data) disk_op((blockno), (data), 0)
#define disk_write(blockno, data) disk_op((blockno), (data), 1)

// ------------------ your code --------------------
bool has_init = 0;
struct sfs_fs file_system;

#define block_size 4096
#define min(X, Y) ((X) < (Y) ? (X) : (Y))

int my_strlen(const char* s) {
    int len = 0;
    while(s[len]) len++;
    return len;
}

int my_strcmp(const char* s1, const char* s2) {
    while(*s1 && *s2 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

void my_strcpy(char* dest, const char* src) {
    while(*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

int hash_func(uint32_t blockno) {
    return blockno % 512;
}

void buffer_pin(int blockno) {
    int index = hash_func(blockno);
    struct list_node* node = file_system.inode_list[index];
    while(node) {
        if(node->block->blockno == blockno) {
            node->block->pin_count++;
            return;
        }
        node = node->next;
    }
}

void buffer_unpin(int blockno) {
    int index = hash_func(blockno);
    struct list_node* node = file_system.inode_list[index];
    while(node) {
        if(node->block->blockno == blockno) {
            if(node->block->pin_count > 0) {
                node->block->pin_count--;
            }
            return;
        }
        node = node->next;
    }
}

void* buffer_read(uint32_t blockno, bool is_inode) {
    int index = hash_func(blockno);
    struct list_node* node = file_system.inode_list[index];
    while(node) {
        if(node->block->blockno == blockno) {
            node->block->pin_count++;
            return is_inode ? (void *)node->block->block.din : (void *)node->block->block.block;
        }
        node = node->next;
    }
    // not found in cache
    struct sfs_memory_block* new_block = (struct sfs_memory_block *)kmalloc(sizeof(struct sfs_memory_block));
    new_block->is_inode = is_inode;
    new_block->blockno = blockno;
    new_block->dirty = 0;
    new_block->pin_count = 1;
    if(is_inode) {
        new_block->block.din = (struct sfs_inode *)kmalloc(block_size);
        disk_read(blockno, (uint8_t *)new_block->block.din);
    } else {
        new_block->block.block = (char *)kmalloc(block_size);
        disk_read(blockno, (uint8_t *)new_block->block.block);
    }
    // insert to cache list
    struct list_node* new_node = (struct list_node *)kmalloc(sizeof(struct list_node));
    new_node->block = new_block;
    new_node->next = file_system.inode_list[index];
    new_node->prev = NULL;
    if(file_system.inode_list[index]) {
        file_system.inode_list[index]->prev = new_node;
    }
    file_system.inode_list[index] = new_node;
    return is_inode ? (void *)new_block->block.din : (void *)new_block->block.block;
}


void buffer_writeback(uint32_t blockno) {
    int index = hash_func(blockno);
    struct list_node* node = file_system.inode_list[index];
    while(node) {
        if(node->block->blockno == blockno) {
            if(node->block->dirty && node->block->pin_count == 0) {
                if(node->block->is_inode) {
                    disk_write(blockno, (uint8_t *)node->block->block.din);
                } else {
                    disk_write(blockno, (uint8_t *)node->block->block.block);
                }
                node->block->dirty = 0;
            }
            if(node->block->pin_count == 0) {
                // free cache
                if(node->block->is_inode) {
                    kfree(node->block->block.din);
                } else {
                    kfree(node->block->block.block);
                }
                kfree(node->block);
                // remove from list
                if(node->prev) {
                    node->prev->next = node->next;
                } else {
                    file_system.inode_list[index] = node->next;
                }
                if(node->next) {
                    node->next->prev = node->prev;
                }
                kfree(node);
            }
            return;
        }
        node = node->next;
    }
}

void inode_writeback(struct sfs_inode* din) {
    if(din->blocks <= SFS_NDIRECT + 1) {
        for(int i = 0; i < (int)din->blocks; i++){
            int blockno = din->direct[i];
            buffer_writeback(blockno);
        }
    } 
    else {
        for(int i = 0; i < SFS_NDIRECT + 1; i++){
            int blockno = din->direct[i];
            buffer_writeback(blockno);
        }
        // 间接块
        int indirect_blockno = din->indirect;
        if(indirect_blockno != 0){
            uint32_t* indirect_block = (uint32_t*)buffer_read(indirect_blockno, 0);
            int indirect_count = din->blocks - SFS_NDIRECT - 1;
            for(int i = 0; i < indirect_count; i++){
                int blockno = indirect_block[i];
                buffer_writeback(blockno);
            }
            buffer_unpin(indirect_blockno);
            buffer_writeback(indirect_blockno);
        }
    }
}

void buffer_set_dirty(uint32_t blockno) {
    int index = hash_func(blockno);
    struct list_node* node = file_system.inode_list[index];
    while(node) {
        if(node->block->blockno == blockno) {
            node->block->dirty = 1;
            return;
        }
        node = node->next;
    }
}

int new_block() {
    int byte_index = 0;
    int bit_index = 0;
    int i = 0;
    while(file_system.freemap_list[i]) {
        struct bitmap* bm = file_system.freemap_list[i];
        for(byte_index = 0; byte_index < 4096; byte_index++) {
            if(bm->freemap[byte_index] != (char)0xFF) {
                for(bit_index = 0; bit_index < 8; bit_index++) {
                    if(!(bm->freemap[byte_index] & (1 << bit_index))) {
                        // found free block
                        bm->freemap[byte_index] |= (1 << bit_index);
                        file_system.super.unused_blocks--;
                        file_system.super_dirty = 1;
                        int blockno = i * 4096 * 8 + byte_index * 8 + bit_index;
                        return blockno;
                    }
                }
            }
        }
        i++;
    }
    return -1; // no free block
}

void create_directory(struct sfs_inode* din, int din_ino, int parent_ino) {
    // create . and .. entries
    struct sfs_entry entries[2];
    entries[0].ino = din_ino;
    my_strcpy(entries[0].filename, ".");
    entries[1].ino = parent_ino;
    my_strcpy(entries[1].filename, "..");
    // write entries to data block
    din->size = 2 * sizeof(struct sfs_entry);
    din->type = SFS_DIRECTORY;
    din->links = 1;
    din->blocks = 1;
    int data_blockno = new_block();
    din->direct[0] = data_blockno;
    din->indirect = 0;
    disk_write(data_blockno, (uint8_t *)&entries);
    buffer_set_dirty(din_ino);
}

int sfs_init() {
    if(has_init) {
        return 0;
    }
    // 读取超级块
    disk_read(0, (uint8_t *)&file_system.super);
    // 检查魔数
    if(file_system.super.magic != SFS_MAGIC) {
        return -1;
    }
    // 初始化 freemap
    int freemap_blocks = (file_system.super.blocks + 4096 * 8 - 1) / (4096 * 8);
    for(int i = 0; i < freemap_blocks; i++) {
        file_system.freemap_list[i] = (struct bitmap *)kmalloc(sizeof(struct bitmap));
        disk_read(2 + i, (uint8_t *)file_system.freemap_list[i]);
    }
    file_system.super_dirty = 0;
    // 初始化 inode_list
    for(int i = 0; i < 512; i++) {
        file_system.inode_list[i] = NULL;
    }
    struct sfs_inode* root_inode = (struct sfs_inode *)buffer_read(1, 1);
    if(root_inode->size == 0) {
        // need to create root directory
        create_directory(root_inode, 1, 1);
    }
    buffer_unpin(1);
    has_init = 1;
    return 0;
}


/**
 * 功能: 打开一个文件, 读权限下如果找不到文件，则返回一个小于 0 的值，表示出错，写权限如果没有找到文件，则创建该文件（包括缺失路径）
 * @path : 文件路径 (绝对路径)
 * @flags: 读写权限 (read, write, read | write)
 * @ret  : file descriptor (fd), 每个进程根据 fd 来唯一的定位到其一个打开的文件
 *         正常返回一个大于 0 的 fd 值, 其他情况表示出错
 */
int sfs_open(const char* path, uint32_t flags) {
    if(!has_init) {
        sfs_init();    
    }
    if(path[0] != '/') {
        return -1;  // only support absolute path
    }
    struct sfs_inode* current_inode = (struct sfs_inode *)buffer_read(1, 1); // start from root inode
    struct sfs_inode* parent_inode = NULL;
    struct sfs_inode* next_inode = NULL;
    uint32_t current_ino = 1;
    uint32_t parent_ino = 1;
    uint32_t next_ino = 1;
    int index = 1;
    while(path[index] != '\0') {
        // extract next component
        char component[SFS_MAX_FILENAME_LEN + 1];
        int comp_len = 0;
        while(path[index] != '/' && path[index] != '\0' && comp_len < SFS_MAX_FILENAME_LEN) {
            component[comp_len++] = path[index++];
        }
        component[comp_len] = '\0';
        if(path[index] == '/') {
            index++;
        }
        // search component in current directory
        if(current_inode->type != SFS_DIRECTORY) {
            buffer_unpin(current_ino);
            return -1; // not a directory
        }
        bool found = 0;
        int entries_per_block = block_size / sizeof(struct sfs_entry);
        for(int i = 0; i < current_inode->blocks; i++) {
            uint32_t blockno;
            if(i < SFS_NDIRECT + 1) {
                blockno = current_inode->direct[i];
            } else {
                uint32_t* indirect_block = (uint32_t *)buffer_read(current_inode->indirect, 0);
                blockno = indirect_block[i - SFS_NDIRECT - 1];
                buffer_unpin(current_inode->indirect);
            }
            struct sfs_entry* block_entries = (struct sfs_entry *)buffer_read(blockno, 0);
            int entries_in_block = min(entries_per_block, 
                (current_inode->size - i * entries_per_block * sizeof(struct sfs_entry)) / sizeof(struct sfs_entry));
            for(int j = 0; j < entries_in_block; j++) {
                if(my_strcmp(block_entries[j].filename, component) == 0) {
                    found = 1;
                    next_ino = block_entries[j].ino;
                    break;
                }
            }
            buffer_unpin(blockno);
            if(found) {
                break;
            }
        }
        if(!found) {
            // not found
            if((flags & SFS_FLAG_WRITE) != 0) {
                int new_ino = new_block();
                struct sfs_inode* new_inode = (struct sfs_inode *)buffer_read(new_ino, 1);
                if(path[index] != '\0') {
                    // create directory
                    create_directory(new_inode, new_ino, current_ino);
                } else {
                    // create file
                    new_inode->size = 0;
                    new_inode->type = SFS_FILE;
                    new_inode->links = 1;
                    new_inode->blocks = 0;
                    new_inode->indirect = 0;
                    buffer_set_dirty(new_ino);
                }
                // add entry to current directory
                struct sfs_entry new_entry;
                new_entry.ino = new_ino;
                my_strcpy(new_entry.filename, component);
                // write entry to data block
                int entry_index = current_inode->size / sizeof(struct sfs_entry);
                int block_index = entry_index / entries_per_block;
                int entry_offset = entry_index % entries_per_block;
                uint32_t blockno;
                if(block_index < SFS_NDIRECT + 1) {
                    if(block_index >= current_inode->blocks) {
                        // need to allocate new data block
                        int new_blockno = new_block();
                        current_inode->direct[block_index] = new_blockno;
                        current_inode->blocks++;
                    }
                    blockno = current_inode->direct[block_index];
                } 
                else {
                    if(current_inode->indirect == 0) {
                        // need to allocate indirect block
                        int new_indirect_blockno = new_block();
                        current_inode->indirect = new_indirect_blockno;
                        uint32_t* indirect_block = (uint32_t *)buffer_read(new_indirect_blockno, 0);
                        memset(indirect_block, 0, block_size);
                        buffer_set_dirty(new_indirect_blockno);
                        buffer_unpin(new_indirect_blockno);
                        buffer_set_dirty(current_ino);
                    }
                    uint32_t* indirect_block = (uint32_t *)buffer_read(current_inode->indirect, 0);
                    if(block_index - SFS_NDIRECT >= current_inode->blocks - SFS_NDIRECT) {
                        // need to allocate new data block
                        int new_blockno = new_block();
                        indirect_block[block_index - SFS_NDIRECT] = new_blockno;
                        current_inode->blocks++;
                        buffer_set_dirty(current_inode->indirect);
                    }
                    blockno = indirect_block[block_index - SFS_NDIRECT];
                    buffer_unpin(current_inode->indirect);
                }
                struct sfs_entry* block_entries = (struct sfs_entry *)buffer_read(blockno, 0);
                block_entries[entry_offset] = new_entry;
                buffer_set_dirty(blockno);
                buffer_unpin(blockno);
                current_inode->size += sizeof(struct sfs_entry);
                buffer_set_dirty(current_ino);
                next_ino = new_ino;
            } 
            else {
                buffer_unpin(current_ino);
                return -1; // file not found
            }
        }
        buffer_unpin(current_ino);
        parent_inode = current_inode;
        parent_ino = current_ino;
        current_inode = buffer_read(next_ino, 1);
        current_ino = next_ino;
    }
    if(current_inode->type == SFS_DIRECTORY){
        buffer_unpin(current_ino);
        return -1; // cannot open directory
    }
    for(int i = 0; i < 16; i++) {
        if(current->fs.fds[i] == NULL) {
            struct file* new_file = (struct file *)kmalloc(sizeof(struct file));
            new_file->inode = current_inode;
            new_file->path = parent_inode;
            new_file->flags = flags;
            new_file->off = 0;
            new_file->ino = current_ino;
            new_file->parent_ino = parent_ino;
            current->fs.fds[i] = new_file;
            return i; // return file descriptor
        }
    }
    // no free file descriptor, release pins
    buffer_unpin(current_ino);
    return -1; // no free file descriptor
}


/**
 * 功能: 关闭一个文件，并将其修改过的内容写回磁盘
 * @fd  : 该进程打开的文件的 file descriptor (fd)
 * @ret : 正确关闭返回 0, 其他情况表示出错
 */
int sfs_close(int fd){
    struct file* f = current->fs.fds[fd];
    if(f == NULL) {
        return -1; // invalid fd
    }
    struct sfs_inode* din = f->inode;
    int ino = f->ino;
    inode_writeback(din);
    buffer_unpin(ino);
    buffer_writeback(ino);
    if(f->path) {
        inode_writeback(f->path);
        buffer_unpin(f->parent_ino);
        buffer_writeback(f->parent_ino);
    }
    if(file_system.super_dirty) {
        // write back super block and freemap
        disk_write(0, (uint8_t *)&file_system.super);
        int freemap_blocks = (file_system.super.blocks + 4096 * 8 - 1) / (4096 * 8);
        for(int i = 0; i < freemap_blocks; i++) {
            if (file_system.freemap_list[i]) {
                disk_write(2 + i, (uint8_t *)file_system.freemap_list[i]->freemap);
            }
        }
        file_system.super_dirty = 0;
    }
    current->fs.fds[fd] = NULL;
    kfree(f);
    return 0;
}

/**
 * 功能  : 根据 fromwhere + off 偏移量来移动文件指针(可参考 C 语言的 fseek 函数功能)
 * @fd  : 该进程打开的文件的 file descriptor (fd)
 * @off : 偏移量
 * @fromwhere : SEEK_SET(文件头), SEEK_CUR(当前), SEEK_END(文件尾)
 * @ret : 表示错误码
 *        = 0 正确返回
 *        < 0 出错
 */
int sfs_seek(int fd, int32_t off, int fromwhere)
{
    struct file* f = current->fs.fds[fd];
    if(f == NULL) {
        return -1; // invalid fd
    }
    struct sfs_inode* din = f->inode;
    int32_t new_off;
    if(fromwhere == SEEK_SET) {
        new_off = off;
    } else if(fromwhere == SEEK_CUR) {
        new_off = (int32_t)f->off + off;
    } else if(fromwhere == SEEK_END) {
        new_off = (int32_t)din->size - off;
    } else {
        return -1; // invalid fromwhere
    }
    if(new_off < 0 || (uint32_t)new_off > din->size) {
        return -1; // cannot seek beyond file size
    }
    f->off = (uint32_t)new_off;
    return 0;
}


/**
 * 功能  : 从文件的文件指针开始读取 len 个字节到 buf 数组中 (结合 sfs_seek 函数使用)，并移动对应的文件指针
 * @fd  : 该进程打开的文件的 file descriptor (fd)
 * @buf : 读取内容的缓存区
 * @len : 要读取的字节的数量
 * @ret : 返回实际读取的字节的个数
 *        < 0 表示出错
 *        = 0 表示已经到了文件末尾，没有能读取的了
 *        > 0 表示实际读取的字节的个数，比如 len = 8，但是文件只剩 5 个字节的情况，就是返回 5
 */
int sfs_read(int fd, char *buf, uint32_t len) {
    struct file *f = current->fs.fds[fd];
    if (f == NULL) {
        return -1;
    }
    
    if (f->inode->type == SFS_DIRECTORY) {
        return -2;
    }
    
    len = min(len, f->inode->size - f->off);
    if (len == 0) {
        return 0;
    }
    
    uint32_t total_read = 0;
    uint32_t remaining = len;
    
    while (remaining > 0) {
        uint32_t block_index = f->off / block_size;
        uint32_t block_offset = f->off % block_size;
        
        if (block_index >= f->inode->blocks) {
            break;
        }
        
        uint32_t blockno;
        if (block_index < SFS_NDIRECT) {
            blockno = f->inode->direct[block_index];
        } else {
            uint32_t *indirect = (uint32_t *)buffer_read(f->inode->indirect, 0);
            blockno = indirect[block_index - SFS_NDIRECT];
            buffer_unpin(f->inode->indirect);
        }
        
        char *block_data = (char *)buffer_read(blockno, 0);
        uint32_t to_read = min(remaining, block_size - block_offset);
        
        memcpy(buf + total_read, block_data + block_offset, to_read);
        
        buffer_unpin(blockno);
        
        total_read += to_read;
        remaining -= to_read;
        f->off += to_read;
    }
    
    return total_read;
}


/**
 * 功能  : 把 buf 数组的前 len 个字节写入到文件的文件指针位置(覆盖)(结合 sfs_seek 函数使用)，并移动对应的文件指针
 * @fd  : 该进程打开的文件的 file descriptor (fd)
 * @buf : 写入内容的缓存区
 * @len : 要写入的字节的数量
 * @ret : 返回实际的字节的个数
 *        < 0 表示出错
 *        >=0 表示实际写入的字节数量
 */
int sfs_write(int fd, char *buf, uint32_t len) {
    struct file *f = current->fs.fds[fd];
    if (f == NULL || !(f->flags & SFS_FLAG_WRITE)) {
        return -1;
    }
    
    if (f->inode->type == SFS_DIRECTORY) {
        return -2;
    }
    
    uint32_t total_written = 0;
    uint32_t remaining = len;
    
    while (remaining > 0) {
        uint32_t block_index = f->off / block_size;
        uint32_t block_offset = f->off % block_size;
        
        // 需要分配新块
        if (block_index >= f->inode->blocks) {
            if (block_index >= SFS_NDIRECT) {
                break; // 不支持间接块扩展
            }
            
            f->inode->direct[block_index] = new_block();
            f->inode->blocks++;
            buffer_set_dirty(f->ino);
        }
        
        uint32_t blockno = f->inode->direct[block_index];
        char *block_data = (char *)buffer_read(blockno, 0);
        
        uint32_t to_write = min(remaining, block_size - block_offset);
        
        memcpy(block_data + block_offset, buf + total_written, to_write);
        
        buffer_set_dirty(blockno);
        buffer_unpin(blockno);
        
        total_written += to_write;
        remaining -= to_write;
        f->off += to_write;
    }
    
    // 更新文件大小
    if (f->off > f->inode->size) {
        f->inode->size = f->off;
        buffer_set_dirty(f->ino);
    }
    
    return total_written;
}


/**
 * 功能    : 获取 path 下的所有文件名，并存储在 files 数组中
 * @path  : 文件夹路径 (绝对路径)
 * @files : 保存该文件夹下所有的文件名
 * @ret   : > 0 表示该文件夹下有多少文件
 *          = 0 表示该 path 是一个文件
 *          < 0 表示出错
 */
int sfs_get_files(const char* path, char* files[])
{
    if(!has_init) {
        sfs_init();    
    }
    if(path[0] != '/') {
        printf("only support absolute path\n");
        return -1;  // only support absolute path
    }
    struct sfs_inode* current_inode = (struct sfs_inode *)buffer_read(1, 1); // start from root inode
    uint32_t current_ino = 1;
    int index1 = 1;
    while(path[index1] != '\0') {
        // extract next component
        char component[SFS_MAX_FILENAME_LEN + 1];
        int comp_len = 0;
        while(path[index1] != '/' && path[index1] != '\0' && comp_len < SFS_MAX_FILENAME_LEN) {
            component[comp_len++] = path[index1++];
        }
        component[comp_len] = '\0';
        if(path[index1] == '/') {
            index1++;
        }
        // search component in current directory
        if(current_inode->type != SFS_DIRECTORY) {
            buffer_unpin(current_ino);
            printf("not a directory\n");
            return -1; // not a directory
        }
        bool found = 0;
        int entries_per_block = block_size / sizeof(struct sfs_entry);
        for(int i = 0; i < current_inode->blocks; i++) {
            uint32_t blockno;
            if(i <= SFS_NDIRECT) {
                blockno = current_inode->direct[i];
            } 
            else {
                uint32_t* indirect_block = (uint32_t *)buffer_read(current_inode->indirect, 0);
                blockno = indirect_block[i - SFS_NDIRECT - 1];
                buffer_unpin(current_inode->indirect);
            }
            char* data_block = (char *)buffer_read(blockno, 0);
            for(int j = 0; j < entries_per_block; j++) {
                if((i * entries_per_block + j) * sizeof(struct sfs_entry) >= current_inode->size) {
                    break; // no more entries
                }
                struct sfs_entry* entry = (struct sfs_entry *)(data_block + j * sizeof(struct sfs_entry));
                if(strcmp(entry->filename, component) == 0) {
                    // found
                    uint32_t next_ino = entry->ino;
                    struct sfs_inode* next_inode = (struct sfs_inode *)buffer_read(next_ino, 1);
                    buffer_unpin(blockno);
                    buffer_unpin(current_ino);
                    current_inode = next_inode;
                    current_ino = next_ino;
                    found = 1;
                    break;
                }
            }
            if(found) {
                break;
            }
            buffer_unpin(blockno);

        }
        if(!found) {
            printf("file not found\n");
            buffer_unpin(current_ino);
            return -1; // file not found
        }
    }
    if(current_inode->type != SFS_DIRECTORY) {
        buffer_unpin(current_ino);
        return -1; // not a directory
    }
    // list files in directory
    int file_count = 0;
    int entries_per_block = block_size / sizeof(struct sfs_entry);
    for(int i = 0; i < current_inode->blocks; i++) {
        uint32_t blockno;
        if(i <= SFS_NDIRECT) {
            blockno = current_inode->direct[i];
        } 
        else {
            uint32_t* indirect_block = (uint32_t *)buffer_read(current_inode->indirect, 0);
            blockno = indirect_block[i - SFS_NDIRECT - 1];
            buffer_unpin(current_inode->indirect);
        }
        struct sfs_entry* data_block = (struct sfs_entry *)buffer_read(blockno, 0);
        for(int j = 0; j < entries_per_block; j++) {
            if((i * entries_per_block + j) * sizeof(struct sfs_entry) >= current_inode->size) {
                break; // no more entries
            }
            my_strcpy(files[file_count], data_block[j].filename);
            file_count++;
        }
        buffer_unpin(blockno);
    }
    buffer_unpin(current_ino);
    return file_count;
}
