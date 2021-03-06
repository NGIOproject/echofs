/*************************************************************************
 * (C) Copyright 2016 Barcelona Supercomputing Center                    *
 *                    Centro Nacional de Supercomputacion                *
 *                                                                       *
 * This file is part of the Echo Filesystem NG.                          *
 *                                                                       *
 * See AUTHORS file in the top level directory for information           *
 * regarding developers and contributors.                                *
 *                                                                       *
 * This library is free software; you can redistribute it and/or         *
 * modify it under the terms of the GNU Lesser General Public            *
 * License as published by the Free Software Foundation; either          *
 * version 3 of the License, or (at your option) any later version.      *
 *                                                                       *
 * The Echo Filesystem NG is distributed in the hope that it will        *
 * be useful, but WITHOUT ANY WARRANTY; without even the implied         *
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR               *
 * PURPOSE.  See the GNU Lesser General Public License for more          *
 * details.                                                              *
 *                                                                       *
 * You should have received a copy of the GNU Lesser General Public      *
 * License along with Echo Filesystem NG; if not, write to the Free      *
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.    *
 *                                                                       *
 *************************************************************************/

 /*
* This software was developed as part of the
* EC H2020 funded project NEXTGenIO (Project ID: 671951)
* www.nextgenio.eu
*/ 


/* C includes */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <iostream>
#include <fuse.h>

#ifdef HAVE_LIBULOCKMGR
extern "C" {
#include <ulockmgr.h>
}
#endif /* HAVE_LIBULOCKMGR */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/fsuid.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif /* HAVE_SETXATTR */

/* C++ includes */
#include <memory>
#include <cstring>
#include <cerrno>
#include <string>
#include <sstream>
#include <functional>

/* internal includes */
#include "settings.h"
#include <metadata/files.h>
#include <metadata/dirs.h>
#include <usr-credentials.h>
#include <command-line.h>
#include "backends.h"
#include <utils.h>

#include "logger.h"
#include "fuse-mount-helper.h"
#include "errors.h"
#include "context.h"
#include "efs-ng.h"

efsng::config::settings m_user_opts;
std::map <std::string, std::string> fastlink;
/**********************************************************************************************************************/
/*   Filesytem operations
 *
 * Most of these should work very similarly to the well known UNIX file system operations.  A major exception is that
 * instead of returning an error in 'errno', the operation should return the negated error value (-errno) directly.
 *
 * All methods are optional, but some are essential for a useful filesystem (e.g. getattr).  Open, flush, release,
 * fsync, opendir, releasedir, fsyncdir, access, create, ftruncate, fgetattr, lock, init and destroy are special purpose
 * methods, without which a full featured filesystem can still be implemented.
 *
 * Almost all operations take a path which can be of any length.
 *
 * Changed in fuse 2.8.0 (regardless of API version) Previously, paths were limited to a length of PATH_MAX.
 *
 * See http://fuse.sourceforge.net/wiki/ for more information.  There is also a snapshot of the relevant wiki pages in
 * the doc/ folder.
 **********************************************************************************************************************/
struct stat cache;
int count = 0;
unsigned long long steps = 1;
/** Get file attributes. similar to stat() */
#if FUSE_USE_VERSION < 30
static int efsng_getattr(const char* pathname, struct stat* stbuf){
#else
static int efsng_getattr(const char* pathname, struct stat* stbuf, struct fuse_file_info* file_info){
#endif

    LOGGER_DEBUG("stat(\"{}\")", pathname);
    LOGGER_TRACE("stat:{}:{}:{}", 
            fuse_get_context()->pid, syscall(__NR_gettid), 
            pathname);
    const char* path = fastlink.count(pathname) >0 ? fastlink[pathname].c_str() : pathname;
    efsng::context* efsng_ctx = (efsng::context*) fuse_get_context()->private_data;
    const auto & kv = efsng_ctx->m_backends.begin();
    const auto& backend_ptr = kv->second; 
    return backend_ptr->do_stat(path, *stbuf);  
}

/** Read the target of a symbolic link */
static int efsng_readlink(const char* pathname, char* buf, size_t bufsiz){
    return -EOPNOTSUPP;

    auto old_credentials = efsng::assume_user_credentials();
    int res = readlink(pathname, buf, bufsiz - 1);
    efsng::restore_credentials(old_credentials);

    if(res == -1){
        return -errno;
    }

    /* the buffer needs to be filled with a null-terminated string */
    buf[res] = '\0';

    return 0;
}

/** Create a node for all non-directory, non-symlink nodes */
static int efsng_mknod(const char* pathname, mode_t mode, dev_t dev){

    return -EOPNOTSUPP;

    auto old_credentials = efsng::assume_user_credentials();
    int res = mknod(pathname, mode, dev);
    efsng::restore_credentials(old_credentials);

    if(res == -1){
        return -errno;
    }

    return 0;
}

/** Create a directory */
static int efsng_mkdir(const char* pathname, mode_t mode){

    efsng::context* efsng_ctx = (efsng::context*) fuse_get_context()->private_data;
    const auto & kv = efsng_ctx->m_backends.begin();
    const auto& backend_ptr = kv->second; 
    return backend_ptr->do_mkdir(pathname, mode);
}

/** Remove a file */
static int efsng_unlink(const char* pathname){

    LOGGER_DEBUG("unlink(\"{}\")", pathname);
    LOGGER_TRACE("unlink:{}:{}:{}", 
            fuse_get_context()->pid, syscall(__NR_gettid), 
            pathname);
    const char* path = fastlink.count(pathname) >0 ? fastlink[pathname].c_str() : pathname;
    fastlink.erase(pathname);
    efsng::context* efsng_ctx = (efsng::context*) fuse_get_context()->private_data;
    const auto & kv = efsng_ctx->m_backends.begin();
    const auto& backend_ptr = kv->second; 
    return backend_ptr->do_unlink(path);
}

/** Remove a directory */
static int efsng_rmdir(const char* pathname){

    efsng::context* efsng_ctx = (efsng::context*) fuse_get_context()->private_data;
    const auto & kv = efsng_ctx->m_backends.begin();
    const auto& backend_ptr = kv->second; 
    const char* path = fastlink.count(pathname) >0 ? fastlink[pathname].c_str() : pathname;
    return backend_ptr->do_rmdir(path);
}

/** Create a symbolic link */
static int efsng_symlink(const char* oldpath, const char* newpath){

    //return -EOPNOTSUPP;
    fastlink[oldpath] = newpath;
    return 0;
}

/** Rename a file */
#if FUSE_USE_VERSION < 30
static int efsng_rename(const char* oldpath, const char* newpath){
#else
static int efsng_rename(const char* oldpath, const char* newpath, unsigned int flags){
#endif

    LOGGER_DEBUG("rename(\"{}\",\"{}\")", oldpath, newpath);

    efsng::context* efsng_ctx = (efsng::context*) fuse_get_context()->private_data;
    const auto & kv = efsng_ctx->m_backends.begin();
    const auto& backend_ptr = kv->second; 

    return backend_ptr->do_rename(oldpath,newpath);
}

/** Create a hard link to a file */
static int efsng_link(const char* oldpath, const char* newpath){
    return -EOPNOTSUPP;
    auto old_credentials = efsng::assume_user_credentials();

    int res = link(oldpath, newpath);

    efsng::restore_credentials(old_credentials);

    if(res == -1){
        return -errno;
    }

    return 0;
}

/** Change the permissions bits of a file */
#if FUSE_USE_VERSION < 30
static int efsng_chmod(const char* pathname, mode_t mode){
#else
static int efsng_chmod(const char* pathname, mode_t mode, struct fuse_file_info* file_info){
#endif


    LOGGER_DEBUG("chmod(\"{}\" {} )", pathname, mode );

    efsng::context* efsng_ctx = (efsng::context*) fuse_get_context()->private_data;
    const auto & kv = efsng_ctx->m_backends.begin();
    const auto& backend_ptr = kv->second; 
   
    return backend_ptr->do_chmod(pathname, mode);
}

/** Change the owner and group of a file */
#if FUSE_USE_VERSION < 30
static int efsng_chown(const char* pathname, uid_t owner, gid_t group){
#else
static int efsng_chown(const char* pathname, uid_t owner, gid_t group, struct fuse_file_info* file_info){
#endif

    LOGGER_DEBUG("chown(\"{}\" {} )", pathname, owner, group );
 
    efsng::context* efsng_ctx = (efsng::context*) fuse_get_context()->private_data;
    const auto & kv = efsng_ctx->m_backends.begin();
    const auto& backend_ptr = kv->second; 
    return backend_ptr->do_chown(pathname, owner, group);
    
    return 0;
}

/** Change the size of a file */
#if FUSE_USE_VERSION < 30
static int efsng_truncate(const char* pathname, off_t length){
#else
static int efsng_truncate(const char* pathname, off_t length, struct fuse_file_info* file_info){
#endif

    int res = 0;
//    return -EOPNOTSUPP;
    if ((long)length < 0) return -EINVAL;
#if FUSE_USE_VERSION < 30
    efsng::context* efsng_ctx = (efsng::context*) fuse_get_context()->private_data;
    const auto & kv = efsng_ctx->m_backends.begin();
    const auto& backend_ptr = kv->second;
    auto ptr = backend_ptr->find(pathname);

    if (ptr == backend_ptr->end()) {
       return -ENOENT;
    }
    auto p_file = ptr->second.get();
    
    p_file->truncate(length);
#else
    if(file_info != NULL){
        auto file_record = (efsng::File*) file_info->fh;
        auto p_file =  file_record->get_ptr();
        p_file->truncate(length);
        }
    
#endif

    if(res == -1){
        return -errno;
    }

    return 0;
}

/** 
 * File open operation
 *
 * No creation (O_CREAT, O_EXCL) and by default also no * truncation (O_TRUNC) flags will be passed to open(). If an
 * * application specifies O_TRUNC, fuse first calls truncate() * and then open(). Only if 'atomic_o_trunc' has been
 * * specified and kernel version is 2.6.24 or later, O_TRUNC is * passed on to open.
 *
 * Unless the 'default_permissions' mount option is given, open should check if the operation is permitted for the given
 * flags. Optionally open may also return an arbitrary filehandle in the fuse_file_info structure, which will be passed
 * to all file operations.
 */
static int efsng_open(const char* pathname, struct fuse_file_info* file_info){

#ifdef __TEST_DIRECT_IO__
    file_info->direct_io = 1;
#endif // __TEST_DIRECT_IO__
    
    efsng::context* efsng_ctx = (efsng::context*) fuse_get_context()->private_data;
    const auto & kv = efsng_ctx->m_backends.begin();
    const auto& backend_ptr = kv->second; 
    
    LOGGER_DEBUG ("OPEN {}", pathname);
    LOGGER_TRACE("open:{}:{}:{}", 
            fuse_get_context()->pid, syscall(__NR_gettid), 
            pathname);

    const char* path = fastlink.count(pathname) >0 ? fastlink[pathname].c_str() : pathname;
    auto ret = backend_ptr->find(path);

    if (ret == backend_ptr->end()) return -ENOENT;

    std::shared_ptr<efsng::backend::file> ptr (ret->second);
    int flags = 0;

    if(file_info->flags & O_RDONLY){
        flags |= O_RDONLY;
    }

    if(file_info->flags & O_WRONLY){
        flags |= O_WRONLY;
    }

    if(file_info->flags & O_RDWR){
        flags |= O_RDWR;
    }

    /* cache the inode, fd, and flags to reuse them later */
    struct stat st;
    ptr->stat(st);

    // XXX this means that each "open()" creates a File record 
    // XXX WARNING: records ARE NOT protected by a mutex yet
    auto file_record = new efsng::File(st.st_ino, 42, flags);
    file_record->set_ptr(ptr);
    file_info->fh = (uint64_t) file_record;

//    file_info->direct_io = 1;

    return 0;
}


/** 
 * Read data from an open file
 *
 * Read should return exactly the number of bytes requested except on EOF or error, otherwise the rest of the data will
 * be substituted with zeroes.   An exception to this is when the 'direct_io' mount option is specified, in which case
 * the return value of the read system call will reflect the return value of this operation.
 */
static int efsng_read(const char* pathname, char* buf, size_t count, off_t offset, struct fuse_file_info* file_info){

    (void) pathname;

    auto file_record = (efsng::File*) file_info->fh;

    int fd = file_record->get_fd();

    int res = pread(fd, buf, count, offset);

    if(res == -1){
        return -errno;
    }

    return res;
}

/** 
 * Write data to an open file
 *
 * Write should return exactly the number of bytes requested except on error.    An exception to this is when the
 * 'direct_io' mount option is specified (see read operation).
 */
static int efsng_write(const char* pathname, const char* buf, size_t count, off_t offset, 
                       struct fuse_file_info* file_info){
return 0;
    (void) pathname;

    auto file_record = (efsng::File*) file_info->fh;

    int fd = file_record->get_fd();

    int res = pwrite(fd, buf, count, offset);

    if(res == -1){
        return -errno;
    }

    return res;
}

/** Get filesystem statistics */
static int efsng_statfs(const char* pathname, struct statvfs* buf){

    auto old_credentials = efsng::assume_user_credentials();

    int res = statvfs(pathname, buf);

    efsng::restore_credentials(old_credentials);

    if(res == -1){
        return -errno;
    }

    return 0;
}

/** 
 * Possibly flush cached data
 *
 * BIG NOTE: This is not equivalent to fsync().  It's not a
 * request to sync dirty data.
 *
 * Flush is called on each close() of a file descriptor.  So if a * filesystem wants to return write errors in close()
 * and the file has cached dirty data, this is a good place to write back data and return any errors.  Since many
 * applications ignore close() errors this is not always useful.
 *
 * NOTE: The flush() method may be called more than once for each open().  This happens if more than one file descriptor
 * refers to an opened file due to dup(), dup2() or fork() calls.   It is not possible to determine if a flush is final,
 * so each flush should be treated equally.  Multiple write-flush sequences are relatively rare, so this shouldn't be
 * a problem.
 *
 * Filesystems shouldn't assume that flush will always be called after some writes, or that if will be called at all.
 */

static int efsng_flush(const char* pathname, struct fuse_file_info* file_info){

    (void) pathname;

    LOGGER_TRACE("close:{}:{}:{}", 
            fuse_get_context()->pid, syscall(__NR_gettid), 
            pathname);

    auto file_record = (efsng::File*) file_info->fh;

//     int fd = file_record->get_fd();
//     if (fd != 42) {
// 
//         /* This is called from every close on an open file, so call the
//            close on the underlying filesystem.  But since flush may be
//            called multiple times for an open file, this must not really
//            close the file.  This is important if used on a network
//            filesystem like NFS which flush the data/metadata on close() */
//         int res = close(dup(fd));
// 
//         if (res == -1){
//             return -errno;
//         }
//     }

    return 0;
}

/** 
 * Release an open file
 *
 * Release is called when there are no more references to an open file: all file descriptors are closed and all
 * memory mappings are unmapped.
 *
 * For every open() call there will be exactly one release() call with the same flags and file descriptor. It is
 * possible to have a file opened more than once, in which case only the last release will mean, that no more
 * reads/writes will happen on the file.  The return value of release is ignored.
 */
static int efsng_release(const char* pathname, struct fuse_file_info* file_info){

    (void) pathname;

    LOGGER_TRACE("close:{}:{}:{}", 
            fuse_get_context()->pid, syscall(__NR_gettid), 
            pathname);

    auto file_record = (efsng::File*) file_info->fh;

//    int fd = file_record->get_fd();
//    if (fd != 42) {
//
//        if(close(fd) == -1){
//            return -errno;
//        }
//
//    }
   
    delete file_record;
    return 0;
}

/** 
 * Synchronize file contents
 *
 * If the datasync parameter is non-zero, then only the user data should be flushed, not the meta data.
 */
static int efsng_fsync(const char* pathname, int is_datasync, struct fuse_file_info* file_info){
    (void) file_info;
    (void) pathname;
    (void) is_datasync;

    LOGGER_TRACE("fsync:{}:{}:{}", 
            fuse_get_context()->pid, syscall(__NR_gettid), 
            pathname);

    // Flush is not necessary, we do not have a cache.

    return 0;
}
// TODO: Do we need to implement extended attributes in the backend?
#ifdef HAVE_SETXATTR
/** Set extended attributes */
static int efsng_setxattr(const char* pathname, const char* name, const char* value, size_t size, int flags){
    return -EOPNOTSUPP;
    auto old_credentials = efsng::assume_user_credentials();

    int res = lsetxattr(pathname, name, value, size, flags);

    efsng::restore_credentials(old_credentials);

    if(res == -1){
        return -errno;
    }

    return 0;
}

/** Get extended attributes */
static int efsng_getxattr(const char* pathname, const char* name, char* value, size_t size){
    return -EOPNOTSUPP;
    auto old_credentials = efsng::assume_user_credentials();

    int res = lgetxattr(pathname, name, value, size);

    efsng::restore_credentials(old_credentials);

    if(res == -1){
        return -errno;
    }

    return 0;
}

/** List extended attributes */
static int efsng_listxattr(const char* pathname, char* listbuf, size_t size){
    return -EOPNOTSUPP;
    auto old_credentials = efsng::assume_user_credentials();

    int res = llistxattr(pathname, listbuf, size);

    efsng::restore_credentials(old_credentials);

    if(res == -1){
        return -errno;
    }

    return 0;
}

/** Remove extended attributes */
static int efsng_removexattr(const char* pathname, const char* name){
    return 0;
    auto old_credentials = efsng::assume_user_credentials();

    int res = lremovexattr(pathname, name);

    efsng::restore_credentials(old_credentials);

    if(res == -1){
        return -errno;
    }

    return 0;
}
#endif /* HAVE_SETXATTR */

/** 
 * Open directory
 *
 * Unless the 'default_permissions' mount option is given, this method should check if opendir is permitted for this
 * directory. Optionally opendir may also return an arbitrary filehandle in the fuse_file_info structure, which will
 * be passed to readdir, closedir and fsyncdir.
 */
static int efsng_opendir(const char* pathname, struct fuse_file_info* file_info){
    (void) file_info;
    (void) pathname;
    LOGGER_TRACE("opendir:{}:{}:{}", 
            fuse_get_context()->pid, syscall(__NR_gettid), 
            pathname);

   /* efsng::context* efsng_ctx = (efsng::context*) fuse_get_context()->private_data;
    const auto & kv = efsng_ctx->m_backends.begin();
    const auto& backend_ptr = kv->second;
  */
  //  struct stat stbuf;
   // backend_ptr->do_stat(pathname,stbuf);
    return 0;
}

/** 
 * Read directory
 *
 * The filesystem may choose between two modes of operation:
 *
 * 1) The readdir implementation ignores the offset parameter, and passes zero to the filler function's offset.  The
 * filler function will not return '1' (unless an error happens), so the whole directory is read in a single readdir
 * operation. This works just like the old getdir() method.
 *
 * 2) The readdir implementation keeps track of the offsets of the directory entries. It uses the offset parameter and
 * always passes non-zero offset to the filler function.  When the buffer is full (or an error happens) the filler
 * function will return '1'.
 */
#if FUSE_USE_VERSION < 30
static int efsng_readdir(const char* pathname, void* buf, fuse_fill_dir_t filler, off_t offset, 
                         struct fuse_file_info* file_info){
#else
static int efsng_readdir(const char* pathname, void* buf, fuse_fill_dir_t filler, off_t offset, 
                         struct fuse_file_info* file_info,
                         enum fuse_readdir_flags flags){
#endif


    LOGGER_DEBUG("readdir called \"{}\" ", pathname);
    LOGGER_TRACE("readdir:{}:{}:{}", 
            fuse_get_context()->pid, syscall(__NR_gettid), 
            pathname);

    efsng::context* efsng_ctx = (efsng::context*) fuse_get_context()->private_data;
    const auto & kv = efsng_ctx->m_backends.begin();
    const auto& backend_ptr = kv->second; 
    const char* path = fastlink.count(pathname) >0 ? fastlink[pathname].c_str() : pathname;
    return backend_ptr->do_readdir(path, buf, filler, offset, file_info);

}

/** Release directory */
static int efsng_releasedir(const char* pathname, struct fuse_file_info* file_info){

    (void) pathname;
    (void) file_info;
    LOGGER_TRACE("closedir:{}:{}:{}", 
            fuse_get_context()->pid, syscall(__NR_gettid), 
            pathname);

    /*
    auto dir_record = (efsng::Directory*) file_info->fh;

    if(closedir(dir_record->get_dirp()) == -1){
        return -errno;
    }

    delete dir_record;
    */
    return 0;
}

/** 
 * Synchronize directory contents
 *
 * If the datasync parameter is non-zero, then only the user data should be flushed, not the metadata
 */
static int efsng_fsyncdir(const char* pathname, int is_datasync, struct fuse_file_info* file_info){

    (void) pathname;
    (void) is_datasync;
    (void) file_info;
    
    return 0;
}

/**
 * Initialize the filesystem
 *
 * The return value will passed in the private_data field of
 * fuse_context to all file operations and as a parameter to the
 * destroy() method.
 */
#if FUSE_USE_VERSION < 30
static void* efsng_init(struct fuse_conn_info *conn) {
#else
static void* efsng_init(struct fuse_conn_info *conn, struct fuse_config* cfg) {
#endif

    (void) conn;

#if FUSE_USE_VERSION >= 30

#ifdef __TEST_WRITEBACK_CACHE__
    if(conn->capable & FUSE_CAP_WRITEBACK_CACHE) {
        conn->want |= FUSE_CAP_WRITEBACK_CACHE;
    } 
    else {
        std::cerr << "WARNING: Writeback cache not supported\n";
    }
#endif // __TEST_WRITEBACK_CACHE__

    conn->want |= FUSE_CAP_SPLICE_READ;
    conn->want |= FUSE_CAP_SPLICE_WRITE;
    conn->want |= FUSE_CAP_SPLICE_MOVE;

    cfg->use_ino = 1;
#endif


    auto efsng_ctx = (efsng::context*) fuse_get_context()->private_data;
    if (efsng_ctx == NULL){
        efsng_ctx = new efsng::context(m_user_opts); 
    }

    try {
        efsng_ctx->initialize();
    } 
    catch(const std::exception& e) {
        if(efsng::logger::get_global_logger() != nullptr) {
            LOGGER_ERROR(e.what());
        }
        else {
            std::cerr << e.what() << "\n";
        }

        // WARNING! trigger_shutdown() does not stop execution!
        efsng_ctx->trigger_shutdown();
        exit(EXIT_FAILURE);
    }
    return (void*) efsng_ctx;
}

/**
 * Clean up the filesystem. Called on filesystem exit.
 */
static void efsng_destroy(void *) {

    efsng::context* efsng_ctx = (efsng::context*) fuse_get_context()->private_data;

    // this will block until all pending tasks have finished so that we can safely
    // destroy the filesystem structures when we return from efsng_destroy() to
    // fuse_custom_mounter()
    
    efsng_ctx->teardown();
    delete efsng_ctx;
}


/**
 * Check file access permissions
 *
 * This will be called for the access() system call.  If the 'default_permissions' mount option is given, this method is
 * not called.
 *
 * This method is not called under Linux kernel versions 2.4.x
 */
static int efsng_access(const char* pathname, int mode){

   
    LOGGER_DEBUG("access called \"{}\" {} ", pathname, mode);
    LOGGER_TRACE("access:{}:{}:{}", 
            fuse_get_context()->pid, syscall(__NR_gettid), 
            pathname);

    /* Search the backends */
    efsng::context* efsng_ctx = (efsng::context*) fuse_get_context()->private_data;
    const auto & kv = efsng_ctx->m_backends.begin();
    const auto& backend_ptr = kv->second; 
    struct stat stbuf;
    const char* path = fastlink.count(pathname) >0 ? fastlink[pathname].c_str() : pathname;
    auto err = backend_ptr->do_stat(path,stbuf);
    if (err != 0) return -ENOENT;
    
    return err;
}

/** 
 * Create and open a file
 *
 * If the file does not exist, first create it with the specified mode, and then open it.
 *
 * If this method is not implemented or under Linux kernel versions earlier than 2.6.15, the mknod() and open() methods
 * will be called instead.
 */
static int efsng_create(const char* pathname, mode_t mode, struct fuse_file_info* file_info){

#ifdef __TEST_DIRECT_IO__
    file_info->direct_io = 1;
#endif // __TEST_DIRECT_IO__

    LOGGER_DEBUG("create called \"{}:{}\" ", pathname, file_info->flags);
    LOGGER_TRACE("create:{}:{}:{}", 
            fuse_get_context()->pid, syscall(__NR_gettid), 
            pathname);

    /* Search the backends */

    efsng::context* efsng_ctx = (efsng::context*) fuse_get_context()->private_data;
    const auto & kv = efsng_ctx->m_backends.begin();
    const auto& backend_ptr = kv->second; 
    std::shared_ptr <efsng::backend::file> ptr;
    auto ret = backend_ptr->do_create(pathname, mode, ptr);
    struct stat st;
    
    ptr->stat(st);
    int flags = 0;

    if(file_info->flags & O_RDONLY){
        flags |= O_RDONLY;
    }

    if(file_info->flags & O_WRONLY){
        flags |= O_WRONLY;
    }

    if(file_info->flags & O_RDWR){
        flags |= O_RDWR;
    }

    file_info->flags = flags;

    auto file_record = new efsng::File(st.st_ino, 42, file_info->flags);
    // TODO : We should have a mutex ? to set boot
    file_record->set_ptr(ptr);
    file_info->fh = (uint64_t) file_record;

    return ret;
}

#if FUSE_USE_VERSION < 30
/**
 * Change the size of an open file
 *
 * This method is called instead of the truncate() method if the truncation was invoked from an ftruncate() system call.
 *
 * If this method is not implemented or under Linux kernel versions earlier than 2.6.15, the truncate() method will be
 * called instead.
 */
static int efsng_ftruncate(const char* pathname, off_t length, struct fuse_file_info* file_info){

    (void) pathname;
    
    LOGGER_DEBUG("truncate(\"{}\", {})", pathname, length);
    LOGGER_TRACE("truncate:{}:{}:{}", 
            fuse_get_context()->pid, syscall(__NR_gettid), 
            pathname);

    auto file_record = (efsng::File*) file_info->fh;
    if (file_info->flags & O_RDONLY) return -EINVAL;
    auto ptr = file_record->get_ptr();
    ptr->truncate(length);

    return 0;
}

/**
 * Get attributes from an open file
 *
 * This method is called instead of the getattr() method if the file information is available.
 *
 * Currently this is only called after the create() method if that is implemented (see above).  Later it may be called
 * for invocations of fstat() too.
 */
static int efsng_fgetattr(const char* pathname, struct stat* stbuf, struct fuse_file_info* file_info){

    (void) pathname;

   // efsng::context* efsng_ctx = (efsng::context*) fuse_get_context()->private_data;

    LOGGER_DEBUG("fstat(\"{}\")", pathname);

    auto file_record = (efsng::File*) file_info->fh;
    auto ptr = file_record->get_ptr();
    ptr->stat(*stbuf);
    return 0;

}
#endif /* FUSE_USE_VERSION < 30 */

#ifdef HAVE_LIBULOCKMGR
/**
 * Perform POSIX file locking operation
 *
 * The cmd argument will be either F_GETLK, F_SETLK or F_SETLKW.
 *
 * For the meaning of fields in 'struct flock' see the man page for fcntl(2).  The l_whence field will always be set to
 * SEEK_SET.
 *
 * For checking lock ownership, the 'fuse_file_info->owner' argument must be used.
 *
 * For F_GETLK operation, the library will first check currently held locks, and if a conflicting lock is found it will
 * return information without calling this method.  This ensures, that for local locks the l_pid field is correctly
 * filled in.  The results may not be accurate in case of race conditions and in the presence of hard links, but it's
 * unlikely that an application would rely on accurate GETLK results in these cases.  If a conflicting lock is not
 * found, this method will be called, and the filesystem may fill out l_pid by a meaningful value, or it may leave this
 * field zero.
 *
 * For F_SETLK and F_SETLKW the l_pid field will be set to the pid of the process performing the locking operation.
 *
 * Note: if this method is not implemented, the kernel will still allow file locking to work locally.  Hence it is only
 * interesting for network filesystems and similar.
 */
static int efsng_lock(const char* pathname, struct fuse_file_info* file_info, int cmd, struct flock* flock){

    (void) pathname;
    return -EOPNOTSUPP;

    auto file_record = (efsng::File*) file_info->fh;

    int fd = file_record->get_inode();

    //int res = ulockmgr_op(fd, cmd, flock, &file_info->lock_owner, sizeof(file_info->lock_owner));
    int res = 0;
    /* ulockmgr_op returns 0 on success and -errno on error */
    return res;
}
#endif /* HAVE_LIBULOCKMGR */

#ifdef HAVE_UTIMENSAT
/**
 * Change the access and modification times of a file with nanosecond resolution
 *
 * This supersedes the old utime() interface.  New applications should use this.
 *
 * See the utimensat(2) man page for details.
 */
#if FUSE_USE_VERSION < 30
static int efsng_utimens(const char* pathname, const struct timespec tv[2]){
#else
static int efsng_utimens(const char* pathname, const struct timespec tv[2], struct fuse_file_info* file_info){
#endif

    LOGGER_DEBUG("utimens called \"{}\" ", pathname);

    /* Search the backends */
    efsng::context* efsng_ctx = (efsng::context*) fuse_get_context()->private_data;
    const auto & kv = efsng_ctx->m_backends.begin();
    const auto& backend_ptr = kv->second; 
    const char* path = fastlink.count(pathname) >0 ? fastlink[pathname].c_str() : pathname;   
    auto ptr = backend_ptr->find(path);
   
    if (ptr == backend_ptr->end()) {     
        return -ENOENT;
    }
    auto p_file = ptr->second.get();
    struct stat stbuf;
    p_file->stat(stbuf);

    stbuf.st_atime = tv[0].tv_sec ;
    stbuf.st_mtime = tv[1].tv_sec;

    p_file->save_attributes(stbuf);
        
    return 0;
}
#endif /* HAVE_UTIMENSAT */

/**
 * Map block index within file to block index within device
 *
 * Note: This makes sense only for block device backed filesystems mounted with the 'blkdev' option
 */
static int efsng_bmap(const char* pathname, size_t blocksize, uint64_t* idx){

    (void) pathname;
    (void) blocksize;
    (void) idx;

    return -EOPNOTSUPP;
}

/**
 * Ioctl
 *
 * flags will have FUSE_IOCTL_COMPAT set for 32bit ioctls in 64bit environment.  The size and direction of data is
 * determined by _IOC_*() decoding of cmd.  For _IOC_NONE, data will be NULL, for _IOC_WRITE data is out area, for
 * _IOC_READ in area and if both are set in/out area.  In all non-NULL cases, the area is of _IOC_SIZE(cmd) bytes.
 */
static int efsng_ioctl(const char* pathname, int cmd, void* arg, struct fuse_file_info* file_info, unsigned int flags,
                       void* data){

    (void) pathname;
    (void) cmd;
    (void) arg;
    (void) file_info;
    (void) flags;
    (void) data;

    return -EOPNOTSUPP;
}

/** 
 * Poll for IO readiness events
 *
 * Note: If ph is non-NULL, the client should notify when IO readiness events occur by calling fuse_notify_poll() with
 * the specified ph.
 *
 * Regardless of the number of times poll with a non-NULL ph is received, single notification is enough to clear all.
 * Notifying more times incurs overhead but doesn't harm correctness.
 *
 * The callee is responsible for destroying ph with fuse_pollhandle_destroy() when no longer in use.
 */
static int efsng_poll(const char* pathname, struct fuse_file_info* file_info, struct fuse_pollhandle* ph, 
                      unsigned* reventsp){

    (void) pathname;
    (void) file_info;
    (void) ph;
    (void) reventsp;

    return -EOPNOTSUPP;
}


/** 
 * Write contents of buffer to an open file
 *
 * Similar to the write() method, but data is supplied in a generic buffer. Use fuse_buf_copy() to transfer data to the
 * destination.
 */
static int efsng_write_buf(const char* pathname, struct fuse_bufvec* buf, off_t offset, 
                           struct fuse_file_info* file_info){

#ifdef __TEST_NOOP_CALLBACK__
    return fuse_buf_size(buf);
#endif // __TEST_NOOP_CALLBACK__

#ifdef __EFS_TIMING__
    auto t0 = std::chrono::steady_clock::now();
#endif

    auto file_record = (efsng::File*) file_info->fh;
    auto file_ptr = file_record->get_ptr();
    //pid_t pid = 0;

    size_t size = fuse_buf_size(buf);

    LOGGER_DEBUG("write({}, {}, {})", pathname, offset, size);
    LOGGER_TRACE("write:{}:{}:{}:{}:{}", 
            fuse_get_context()->pid, syscall(__NR_gettid), 
            pathname, offset, size);

    ssize_t rv = file_ptr->put_data(offset, size, buf);

#ifdef __EFS_TIMING__
    auto t1 = std::chrono::steady_clock::now();
    auto delta = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    LOGGER_INFO("efsng_write_buf({}, {}, {}) took {} usecs", pathname, offset, size, std::to_string(delta));
#endif
    
    
    return rv;
}

/** 
 * Store data from an open file in a buffer
 *
 * Similar to the read() method, but data is stored and returned in a generic buffer.
 *
 * No actual copying of data has to take place, the source file descriptor may simply be stored in the buffer for later
 * data transfer.
 *
 * The buffer must be allocated dynamically and stored at the location pointed to by bufp.  If the buffer contains
 * memory regions, they too must be allocated using malloc().  The allocated memory will be freed by the caller.
 */
static int efsng_read_buf(const char* pathname, struct fuse_bufvec** bufp, size_t size, off_t offset, 
                          struct fuse_file_info* file_info){

    auto file_record = (efsng::File*) file_info->fh;
    auto file_ptr = file_record->get_ptr();

    struct fuse_bufvec* dst;

    dst = (struct fuse_bufvec*) malloc(sizeof(struct fuse_bufvec));

    if(dst == NULL){
        return -ENOMEM;
    }

    LOGGER_DEBUG("read(\"{}\", {}, {})", pathname, offset, size);
    LOGGER_TRACE("read:{}:{}:{}:{}:{}", 
            fuse_get_context()->pid, syscall(__NR_gettid), 
            pathname, offset, size);

    *dst = FUSE_BUFVEC_INIT(size);

    ssize_t rv = file_ptr->get_data(offset, size, dst);

    *bufp = dst;

    return rv;
}

/**
 * Perform BSD file locking operation
 *
 * The op argument will be either LOCK_SH, LOCK_EX or LOCK_UN
 *
 * Nonblocking requests will be indicated by ORing LOCK_NB to the above operations
 *
 * For more information see the flock(2) manual page.
 *
 * Additionally fi->owner will be set to a value unique to this open file.  This same value will be supplied to
 * ->release() when the file is released.
 *
 * Note: if this method is not implemented, the kernel will still allow file locking to work locally.  Hence it is only
 * interesting for network filesystems and similar.
 */
static int efsng_flock(const char* pathname, struct fuse_file_info* file_info, int op){

    (void) pathname;
    return -EOPNOTSUPP;
    auto file_record = (efsng::File*) file_info->fh;

    int fd = file_record->get_fd();

    int res = flock(fd, op);
    
    if(res == -1){
        return -errno;
    }

    return 0;
}

#ifdef HAVE_POSIX_FALLOCATE
/**
 * Allocates space for an open file
 *
 * This function ensures that required space is allocated for specified file. If this function returns success then any
 * subsequent write request to specified range is guaranteed not to fail because of lack of space on the file system
 * media.
 */
static int efsng_fallocate(const char* pathname, int mode, off_t offset, off_t length, 
                           struct fuse_file_info* file_info){

    (void) pathname;
      if(mode != 0){
        return -EOPNOTSUPP;
    }
    
    LOGGER_DEBUG("allocate(\"{}\", {}, {})", pathname, offset, length);

    auto file_record = (efsng::File*) file_info->fh;
    auto file_ptr = file_record->get_ptr();
    ssize_t rv = file_ptr->allocate(offset, length);
    
    return rv;
    
    /* only posix_fallocate is supported at the moment */
  
}
#endif /* HAVE_POSIX_FALLOCATE */

/**********************************************************************************************************************/
/*  main point of entry                                                                                               */
/**********************************************************************************************************************/
int main (int argc, char *argv[]){

    std::string exec_name = bfs::basename(argv[0]);

    /* 1. parse command-line arguments */
    //efsng::config::settings user_opts;

    if(argc == 1) {
        cmdline::usage(exec_name, true);
        return EXIT_FAILURE;
    }

    try {
        m_user_opts.from_cmdline(argc, argv);
    } 
    catch(const std::exception& e) {
        std::cerr << exec_name << ": " << e.what() << "\n";
        return EXIT_FAILURE;
    }
    
    /* 2. prepare operations */
    fuse_operations efsng_ops;
    memset(&efsng_ops, 0, sizeof(fuse_operations));

    efsng_ops.getattr = efsng_getattr;
    efsng_ops.readlink = efsng_readlink;
    /* efsng_ops.getdir = efsng_getdir; // deprecated */
    efsng_ops.mknod = efsng_mknod;
    efsng_ops.mkdir = efsng_mkdir;
    efsng_ops.unlink = efsng_unlink;
    efsng_ops.rmdir = efsng_rmdir;
    efsng_ops.symlink = efsng_symlink;
    efsng_ops.rename = efsng_rename;
    efsng_ops.link = efsng_link;
    efsng_ops.chmod = efsng_chmod;
    efsng_ops.chown = efsng_chown;
    efsng_ops.truncate = efsng_truncate;
    /* efsng_ops.utime = efsng_utime; // deprecated */
    efsng_ops.open = efsng_open;
    efsng_ops.read = efsng_read;
    efsng_ops.write = efsng_write;
    efsng_ops.statfs = efsng_statfs;
    efsng_ops.flush = efsng_flush;
    efsng_ops.release = efsng_release;
    efsng_ops.fsync = efsng_fsync;

#ifdef HAVE_SETXATTR
    efsng_ops.setxattr = efsng_setxattr;
    efsng_ops.getxattr = efsng_getxattr;
    efsng_ops.listxattr = efsng_listxattr;
    efsng_ops.removexattr = efsng_removexattr;
#endif /* HAVE_SETXATTR */

    efsng_ops.opendir = efsng_opendir;
    efsng_ops.readdir = efsng_readdir;
    efsng_ops.releasedir = efsng_releasedir;
    efsng_ops.fsyncdir = efsng_fsyncdir;
    efsng_ops.init = efsng_init;
    efsng_ops.destroy = efsng_destroy;
    efsng_ops.access = efsng_access;
    efsng_ops.create = efsng_create;

#if FUSE_USE_VERSION < 30
    efsng_ops.ftruncate = efsng_ftruncate;
    efsng_ops.fgetattr = efsng_fgetattr;
#endif

#ifdef HAVE_LIBULOCKMGR
    efsng_ops.lock = efsng_lock;
#endif /* HAVE_LIBULOCKMGR */

#ifdef HAVE_UTIMENSAT
    efsng_ops.utimens = efsng_utimens;
#endif /* HAVE_UTIMENSAT */

    efsng_ops.bmap = efsng_bmap;
    efsng_ops.ioctl = efsng_ioctl;
    efsng_ops.poll = efsng_poll;
    efsng_ops.write_buf = efsng_write_buf;
    efsng_ops.read_buf = efsng_read_buf;
    efsng_ops.flock = efsng_flock;

#ifdef HAVE_POSIX_FALLOCATE
    efsng_ops.fallocate = efsng_fallocate;
#endif /* HAVE_POSIX_FALLOCATE */
    
    /* 3. set the umask */
    umask(0);

    /* 4. start the FUSE filesystem */
//    int res = efsng::fuse_custom_mounter(m_user_opts, &efsng_ops);

    int res = fuse_main(m_user_opts.m_fuse_argc, 
                        const_cast<char **>(m_user_opts.m_fuse_argv), 
                        &efsng_ops, 
                        NULL);
    
    return res;
}
