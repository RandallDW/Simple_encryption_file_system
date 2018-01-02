# Simple Encryption Stackable File System

## Introduction
In this project, I developed an eXtremely simple enCryption File System (XCFS), which is a stackable file system running on top of an existing file system (e.g., ext4) and provides encryption/description of file contents. For a file created at XCFS, an user can see decrypted file contents only through XCFS; if an user directly accesses the file without using XCFS, he or she will see encrypted file contents

## Running Environment
    * Linux Kernel v4.12

## Packages 
    * Wrapfs source code

The newest Wrapfs only works under linux kernel v4.10

## Folder
    * wrapfs_kernel_v4.12
        - Modified wrapfs source code. This version should
          work under kernel v4.12
    * xcfs
        - extremely simple encryption stackable file system

## Design
All designs are based on wrapfs source code. 

### Add new file operations
    const struct file_operations xcfs_mmap_fops = {
        .llseek             = generic_file_llseek,
        .read_iter          = generic_file_read_iter,
        .write_iter         = generic_file_write_iter,
        .unlocked_ioctl     = xcfs_unlocked_ioctl,
        #ifdef CONFIG_COMPAT
            .compat_ioctl   = xcfs_compat_ioctl,
        #endif
        .mmap               = xcfs_mmap,
        .open               = xcfs_open,
        .flush              = xcfs_flush,
        .release            = xcfs_file_release,
        .fsync              = xcfs_fsync,
        .fasync             = xcfs_fasync,
    };


### Add new address space operations
    const struct address_space_operations xcfs_aops = {
        .direct_IO   = xcfs_direct_IO,
        .readpage    = xcfs_readpage,
        .writepage   = xcfs_writepage,
        .write_begin = xcfs_write_begin,
        .write_end   = xcfs_write_end,
    };

#### Encryption
    * Adding one for each byte
    * Encrypt inside xcfs_readpage
#### Decryption
    * Substracting one for each byte
    * Decrypt inside xcfs_write_end

### Remove wrapfs_fault and replaced with ext4_filemap_fault 
The old wrapfs_fault function existed bugs. 
In wrapfs_fault, the upper layer inode points to NULL. This will cause bug when people try to compile a program with wrapfs.
<br />
Two solutions:

    * Use ext4_filemap_fault
    * Adding page, and let upper layer inode pagetable point to it.

In this project, I used first way to solve this bug. For more details, you could take a look of ecryptfs source code.
     
## Reference
    * Wrapfs (http://wrapfs.filesystems.org/)
    * Ecryptfs (https://github.com/torvalds/linux/tree/master/fs/ecryptf)
    * Stackable File System (https://github.com/abhishekShukla/Linux-Stackable-File-System-)
