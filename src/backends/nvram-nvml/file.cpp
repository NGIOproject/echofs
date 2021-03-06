/*************************************************************************
 * (C) Copyright 2016-2017 Barcelona Supercomputing Center               *
 *                         Centro Nacional de Supercomputacion           *
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


#include <libpmem.h>
#include <fstream>

#include "fuse_buf_copy_pmem.h"
#include <nvram-nvml/file.h>

//#define __PRINT_TREE__

#if defined(__EFS_DEBUG__) && defined(__PRINT_TREE__)
namespace {
void print_tree(const efsng::nvml::segment_tree& tree) {

    std::cerr << "tree {\n";

    for(auto it = tree.begin(); it != tree.end(); ++it) {

        const auto& sptr = it->second;

        std::cerr << it->first << " -> ";

        if(sptr != nullptr) {
            std::cerr << *sptr << "\n";
        }
        else {
            std::cerr << "nullptr" << ";\n";
        }
    }

    std::cerr << "};\n";
}
}
#endif

namespace efsng {
namespace nvml {

/**********************************************************************************************************************/
/* class implementation                                                                                               */
/**********************************************************************************************************************/
file::file() 
    : backend::file(),
      m_segment_size(4096),
      m_segments(0, std::numeric_limits<off_t>::max(), segment_ptr()),
      m_initialized(false)    {
}

file::file(const bfs::path& pool_base, const bfs::path& pathname, const ino_t inode, file::type type,  bool populate) 
    : m_pathname(pathname),
      m_type(type),
      m_alloc_offset(0),
      m_used_offset(0),
      m_segment_size(4096),
      m_segments(0, std::numeric_limits<off_t>::max(), segment_ptr()),
      m_initialized(false) {

      m_pool_subdir = generate_pool_subdir(pool_base, pathname);
   
    if(populate) { //XXX this is probably not needed if we have another constructor
                   // for non-Lustre backed files

        boost::unique_lock<boost::shared_mutex> lock(m_initialized_mutex);
       
         /* generate a subdir to store all pools for this file */

        /* if the pool subdir already exists delete it, since we can't trust it */
        /* TODO: we may need to change this in the future */
        if(bfs::exists(m_pool_subdir)) {
            try {
                remove_all(m_pool_subdir);
            }
            catch(const bfs::filesystem_error& e) {
                throw std::runtime_error(
                        logger::build_message("Error removing stale pool subdir: ", m_pool_subdir, " (", e.what(), ")"));
            }
        }

        if(::mkdir(m_pool_subdir.c_str(), S_IRWXU) != 0) {
            throw std::runtime_error(
                    logger::build_message("Error creating pool subdir: ", m_pool_subdir, " (", strerror(errno), ")"));
        }


        posix::file fd(pathname);
        struct stat stbuf;
        fd.stat(stbuf);
        stbuf.st_ino = inode;
        off_t seg_offset = 0;
        size_t seg_size = efsng::xalign(stbuf.st_size, m_segment_size);      // Preloaded file

        auto sptr = create_segment(seg_offset, seg_size, /*is_gap=*/false);

        append_segments({sptr});

        m_alloc_offset = sptr->m_size;
        m_used_offset = sptr->fill_from(fd);

        save_attributes(stbuf);
        m_initialized = true;
    } else {
        m_segments.build_tree();
    }
}

file::~file() {
 //   std::cerr << "a nvml::file " << m_pathname.string() <<  " instance died...\n" ;
     if(bfs::exists(m_pool_subdir)) {
        try {
             remove_all(m_pool_subdir);
        }
        catch(const bfs::filesystem_error& e) {
            throw std::runtime_error(
                    logger::build_message("Error removing stale pool subdir: ", m_pool_subdir, " (", e.what(), ")")
                    );
        }
     }
}

void file::stat(struct stat& stbuf) const {

    m_alloc_mutex.lock_shared();
    memcpy(&stbuf, &m_attributes, sizeof(stbuf));
    m_alloc_mutex.unlock_shared();
}

void file::save_attributes(struct stat& stbuf) {
    m_alloc_mutex.lock();
    memcpy(&m_attributes, &stbuf, sizeof(m_attributes));
    m_alloc_mutex.unlock();
}

lock_manager::range_lock file::lock_range(off_t start, off_t end, operation op) {

    if(op == operation::read) {
        return m_range_mutex.lock_range(start, end, lock_manager::type::reader);
    }

    assert(op == operation::write);
    return m_range_mutex.lock_range(start, end, lock_manager::type::writer);
}

void file::unlock_range(lock_manager::range_lock& rl) {
    m_range_mutex.unlock_range(rl);
}


size_t file::size() const {

    size_t size = 0;
    
    m_alloc_mutex.lock_shared();

    size = m_attributes.st_size;

    m_alloc_mutex.unlock_shared();

    return size;
}

void file::update_size(size_t size) {

    m_alloc_mutex.lock();

    // another thread may have completed a write beyond ours
    if(m_attributes.st_size < (off_t) size) {
        m_used_offset = size;
        m_attributes.st_size = size;
        m_attributes.st_blocks = size/512;
    }

    m_alloc_mutex.unlock();
}

int file::unload (const std::string name){

    if (m_type == file::type::temporary) return -1;

    file_region_list regions;
    std::ofstream output(name, std::ios::binary);
    auto rl = lock_range(0, size(), efsng::operation::read);

    m_alloc_mutex.lock_shared();

    lookup_data(0, size(), regions);

    m_alloc_mutex.unlock_shared();

    if(regions.count() == 0) {
        goto unlock_and_return_unload;
    }

    /* the FUSE interface forces us to allocate a buffer using malloc() and 
    * memcpy() the requested data in order to return it back to the user. meh */

    for(const auto& r : regions) {
        efsng::data_ptr_t data = r.m_address;
        size_t size = r.m_size;
       
        if(data != NULL) {
            output.write((const char*)data, size);
        }
        else { //TODO ask Alberto
            output.write((const char*)0, size);
        }
    }

    unlock_and_return_unload:
    //XXX if posix_consistency:
    unlock_range(rl);
    output.close();

    return 0;
}

segment_ptr file::create_segment(off_t base_offset, size_t size, bool is_gap) {

    segment_ptr sptr(new segment(m_pool_subdir, base_offset, size, is_gap));
//    sptr->zero_fill(0, sptr->m_size);

    return sptr;
}

void file::append_segments(const segment_list& segments) {

    for(const auto sptr : segments) {
        m_segments.insert_back(sptr->m_offset, sptr->m_offset + sptr->m_size, sptr);
    }

    m_segments.build_tree();

#if defined(__EFS_DEBUG__) && defined(__PRINT_TREE__)
    print_tree(m_segments);
#endif

}

void file::insert_segments(const segment_list& segments) {

    for(const auto sptr : segments) {
        m_segments.insert_front(sptr->m_offset, sptr->m_offset + sptr->m_size, sptr);
    }

    m_segments.build_tree();

#if defined(__EFS_DEBUG__) && defined(__PRINT_TREE__)
    print_tree(m_segments);
#endif

}

// precondition: 
// - m_alloc_mutex locked
void file::fetch_storage(off_t offset, size_t size, file_region_list& regions) {

    if(offset + (off_t) size <= m_alloc_offset) {
        lookup_segments(offset, offset + size, regions);
        return;
    }

    assert(offset <= (off_t) (offset + size));
    assert(m_segments.is_tree_valid());

    // the append may affect several already existing segments: add them to *regions*
    if(offset < m_alloc_offset) {
        lookup_segments(offset, m_alloc_offset, regions);
    }

    // the last (valid) physical segment should start at *m_alloc_offset*
    // and contain a *nullptr*
    assert((*++m_segments.rbegin()).first == m_alloc_offset);
    assert((*++m_segments.rbegin()).second == nullptr);
    m_segment_size = std::min (m_segment_size*2, segment::s_segment_size);
    off_t new_segment_offset = (offset <= m_alloc_offset ?  m_alloc_offset : 
            efsng::align(offset, m_segment_size));
    size_t gap_size = new_segment_offset - m_alloc_offset;
    size_t new_segment_size = efsng::xalign(offset + size, m_segment_size) - new_segment_offset;

    segment_list sl;

    // allocate a gap segment if offset > EOF + s_segment_size
    if(gap_size != 0) {
        assert(offset > m_alloc_offset);
        auto sptr = create_segment(m_alloc_offset, gap_size, /*is_gap=*/true);
        sl.push_back(sptr);
    }

    // allocate a new segment where we will store the data the prompted 
    // the call to extend_alloc
    auto sptr = create_segment(new_segment_offset, new_segment_size, /*is_gap=*/false);
    sl.push_back(sptr);

    append_segments(sl);

    size_t op_delta = (offset <= m_alloc_offset ? 0 : offset - new_segment_offset);
    data_ptr_t s_addr = (data_ptr_t) ((uintptr_t) sptr->data() + op_delta);

    regions.emplace_back(s_addr, offset + size - new_segment_offset, 
            /*is_gap=*/false, /*is_pmem=*/sptr->is_pmem());

    m_alloc_offset = new_segment_offset + new_segment_size;
}

void file::lookup_segments(off_t range_start, off_t range_end, file_region_list& regions) {

    assert(range_start >= 0);
    assert(range_start <= range_end);
    assert(m_segments.is_tree_valid());

    lookup_helper(range_start, range_end, /*alloc_gaps_as_needed=*/true, regions);

}

void file::lookup_data(off_t range_start, off_t range_end, file_region_list& regions) const {

    assert(range_start >= 0);
    assert(range_start <= range_end);
    assert(m_segments.is_tree_valid());

    if(range_end > m_used_offset) {
        range_end = m_used_offset;
    }

    const_cast<file*>(this)->lookup_helper(range_start, range_end, /*alloc_gaps_as_needed=*/false, regions);

}

void file::lookup_helper(off_t range_start, off_t range_end, bool alloc_gaps_as_needed, file_region_list& regions) {

    size_t req_size = range_end - range_start;

    if(req_size == 0) {
        return;
    }

    segment_ptr sptr;
    auto res = m_segments.search_tree(range_start, sptr);

    // range_start must exist, since the segment tree covers from 0 to 
    // std::numeric_limits<off_t>::max(). Also, if a non-allocated offset 
    // is accessed, the associated segment will be nullptr
    assert(res.second == true);

    segment_tree::const_iterator& it = res.first;

    do {
        const auto& s = it->second;

        // out of allocated file
        if(s == nullptr) {
            return;
        }

        off_t s_start = s->m_offset;
        off_t s_end = s_start + s->m_size;
        off_t op_delta = range_start - s_start;

        // this should be guaranteed by search_tree
        assert(op_delta != (off_t) s->m_size);

        ssize_t op_size = std::min({(ssize_t) req_size,
                                    (ssize_t) s->m_size - op_delta});

        if(alloc_gaps_as_needed && s->m_is_gap) {
            off_t seg_offset = efsng::align(range_start, segment::s_segment_size);
            size_t seg_size = (range_start + req_size < s->m_offset + s->m_size) ?
                efsng::xalign(range_start + req_size, segment::s_segment_size) - seg_offset :
                s->m_offset + s->m_size - seg_offset;

            segment_list sl;

            s->allocate(seg_offset, seg_size);
            

            off_t start_gap_offset = s_start;
            size_t start_gap_size = seg_offset - s_start;

            if(start_gap_size != 0) {
                auto sptr = create_segment(start_gap_offset, start_gap_size, /*is_gap=*/true);
                sl.push_back(sptr);
            }

            // update op_delta since we have modified things
            op_delta = range_start - (start_gap_offset + start_gap_size);

            off_t end_gap_offset = seg_offset + seg_size;
            size_t end_gap_size = s_end - (seg_offset + seg_size);

            if(end_gap_size != 0) {
                auto sptr = create_segment(end_gap_offset, end_gap_size, /*is_gap=*/true);
                sl.push_back(sptr);
            }

            insert_segments(sl);
        }

        data_ptr_t s_addr = s->m_is_gap ? 
                            NULL :
                            (data_ptr_t) ((uintptr_t)s->data() + op_delta);

        regions.emplace_back(s_addr, op_size, s->m_is_gap, s->is_pmem());

        if(s_end >= range_end) {
            return;
        }

        range_start = s_end;
        req_size -= op_size;
        ++it;
    } while(true);
}


bfs::path file::generate_pool_subdir(const bfs::path& pool_base, const bfs::path& pathname) const {

    std::stringstream ss;
    ss << std::hex << std::hash<std::string>()(pathname.string());

    return pool_base / bfs::path(ss.str());
}


ssize_t file::get_data(off_t start_offset, size_t size, struct fuse_bufvec* fuse_buffer) {

    size_t n = 0;
    ssize_t rv = 0;
    void* buffer = NULL;
    off_t end_offset = start_offset + size;

    assert(start_offset < end_offset);

    file_region_list regions;

    if (!m_initialized){
         
        /* if the pool subdir already exists delete it, since we can't trust it */
        /* TODO: we may need to change this in the future */
        if(bfs::exists(m_pool_subdir)) {
            try {
                remove_all(m_pool_subdir);
            }
            catch(const bfs::filesystem_error& e) {
                throw std::runtime_error(
                        logger::build_message("Error removing stale pool subdir: ", m_pool_subdir, " (", e.what(), ")"));
            }
        }

        if(::mkdir(m_pool_subdir.c_str(), S_IRWXU) != 0) {
            throw std::runtime_error(
                    logger::build_message("Error creating pool subdir: ", m_pool_subdir, " (", strerror(errno), ")"));
        }
        m_initialized = true;
    }
   
//XXX if posix_consistency:
    auto rl = lock_range(start_offset, end_offset, efsng::operation::read);

    m_alloc_mutex.lock_shared();

    lookup_data(start_offset, end_offset, regions);

    m_alloc_mutex.unlock_shared();

    if(regions.count() == 0) {
        fuse_buffer->buf[0].flags = (fuse_buf_flags) (~FUSE_BUF_IS_FD);
        fuse_buffer->buf[0].mem = NULL;
        fuse_buffer->buf[0].size = 0;

        rv = 0;
        goto unlock_and_return;
    }

#ifndef __TEST_FUSE_READBUF_PATCH__
    /* the FUSE interface forces us to allocate a buffer using malloc() and 
    * memcpy() the requested data in order to return it back to the user. meh */
    buffer = (void*) malloc(regions.total_size());

    if(posix_memalign(&buffer, 512, regions.total_size()) != 0) {
        rv = -ENOMEM;
        goto unlock_and_return;
    }

    if(buffer == NULL) {
        rv = -ENOMEM;
        goto unlock_and_return;
    }
#endif // __TEST_FUSE_READBUF_PATCH__

    for(const auto& r : regions) {
        efsng::data_ptr_t data = r.m_address;
        size_t size = r.m_size;

        if(data != NULL) {
#ifndef __TEST_FUSE_READBUF_PATCH__
            memcpy((void*) ((uintptr_t)buffer + n), (void*) data, size);
#else
            buffer = data;
#endif // __TEST_FUSE_READBUF_PATCH__

            LOGGER_TRACE("nvml_read:{}:{}:{}:{}", 
                    fuse_get_context()->pid, syscall(__NR_gettid), 
                    data, size);
        }
        else {
            memset((void*) ((uintptr_t)buffer + n), 0, size);

            LOGGER_TRACE("nvml_read:{}:{}:0x0:{}", 
                    fuse_get_context()->pid, syscall(__NR_gettid), 
                    size);
        }

        n += size;
    }

    assert(n == regions.total_size());

    fuse_buffer->buf[0].flags = (fuse_buf_flags) (~FUSE_BUF_IS_FD);
    fuse_buffer->buf[0].mem = buffer;
    fuse_buffer->buf[0].size = regions.total_size();


unlock_and_return:
//XXX if posix_consistency:
    unlock_range(rl);

    return rv;
}

#ifdef __TEST_STATIC_BUFFER__
char global_buffer[8*1024*1024];
#endif // __TEST_STATIC_BUFFER__

ssize_t file::put_data(off_t start_offset, size_t size, struct fuse_bufvec* fuse_buffer) {

#ifdef __TEST_SINGLE_BUFFER_WRITES__

    { // single malloc'ed buffer
#ifdef __TEST_STATIC_BUFFER__
        void* buffer = global_buffer;

        assert(size <= sizeof(global_buffer));
#else
        void* buffer = malloc(size);
        assert(buffer);
#endif

        struct fuse_bufvec dst = FUSE_BUFVEC_INIT(size);
        dst.buf[0].flags = (fuse_buf_flags) (~FUSE_BUF_IS_FD);
        dst.buf[0].mem = (void*) buffer;

        return fuse_buf_copy(&dst, fuse_buffer, FUSE_BUF_SPLICE_MOVE);
    }

#else

    // a truncate() call while writing data is a problem: allocated segments
    // that we believe exist may be removed before writing data to them. To avoid 
    // this, all writers (i.e. data adders) try to get a shared_lock on m_dealloc_mutex 
    // before doing anything, whereas truncate() will try to get a unique_lock().
    // Thus, all writers can put data into a file's segments concurrently, without having 
    // to worry about them vanishing
    m_dealloc_mutex.lock_shared();
    if (!m_initialized){
        boost::unique_lock<boost::shared_mutex> lock(m_alloc_mutex);
      
        /* if the pool subdir already exists delete it, since we can't trust it */
        /* TODO: we may need to change this in the future */
        if(bfs::exists(m_pool_subdir)) {
            try {
                remove_all(m_pool_subdir);
            }
            catch(const bfs::filesystem_error& e) {
                throw std::runtime_error(
                        logger::build_message("Error removing stale pool subdir: ", m_pool_subdir, " (", e.what(), ")"));
            }
        }
        if(::mkdir(m_pool_subdir.c_str(), S_IRWXU) != 0) {
            throw std::runtime_error(
                    logger::build_message("Error creating pool subdir (pd): ", m_pool_subdir, " (", strerror(errno), ")"));
        }
        m_initialized = true;
    }
    

    off_t end_offset = start_offset + size;

    file_region_list regions;

    // get segments affected by the write operation
    {
        boost::unique_lock<boost::shared_mutex> lock(m_alloc_mutex);

        // this will allocate any additional segments required
        fetch_storage(start_offset, size, regions);

        // lock released here
    }

    // by this point, the file has enough storage in NVRAM for the data
    // (and even more than that if another thread enlarged it further after 
    // we released the lock). Now we can lock only the affected ranges 
    // rather than the whole file, so that non-overlapping threads can 
    // write data without having to block
    auto rl = lock_range(start_offset, end_offset, efsng::operation::write);

    ssize_t n = 0;
    bool needs_sync = false;

    for(const auto& r : regions) {
        //XXX not all regions need data to be written to them!
        struct fuse_bufvec dst = FUSE_BUFVEC_INIT(r.m_size);

        dst.buf[0].flags = (fuse_buf_flags) (~FUSE_BUF_IS_FD);
        dst.buf[0].mem = (void*) r.m_address;
 
        LOGGER_TRACE("nvml_write:{}:{}:{}:{}:{}", 
                fuse_get_context()->pid, syscall(__NR_gettid), 
                "nvml", r.m_address, r.m_size);

        // copy user data from received in *fuse_buffer* to dst
        // (fuse_buf_copy tracks how much data has been copied)
        if(r.m_is_pmem) {
            n += fuse_buf_copy_pmem(&dst, fuse_buffer, FUSE_BUF_SPLICE_MOVE);
        }
        else {
            n += fuse_buf_copy(&dst, fuse_buffer, FUSE_BUF_SPLICE_MOVE);
        }

        needs_sync |= r.m_is_pmem;
    }

//XXX FIXME this should be done only if fsync() is called!
//    // ensure data is persistent
//    if(needs_sync) {
//        segment::sync_all();
//    }

    // update cached attributes
    update_size(end_offset);

    unlock_range(rl);

    m_dealloc_mutex.unlock_shared();
    return n;

#endif // ! __SINGLE_BUFFER_WRITES__
}

ssize_t file::append_data(off_t start_offset, size_t size, struct fuse_bufvec* fuse_buffer) {
    (void)start_offset;
    (void)size;
    (void)fuse_buffer;

    return 0;
}

ssize_t file::allocate(off_t start_offset, size_t size){
     
    
    if (!m_initialized){
      
        /* if the pool subdir already exists delete it, since we can't trust it */
        /* TODO: we may need to change this in the future */
        if(bfs::exists(m_pool_subdir)) {
            try {
                remove_all(m_pool_subdir);
            }
            catch(const bfs::filesystem_error& e) {
                throw std::runtime_error(
                        logger::build_message("Error removing stale pool subdir: ", m_pool_subdir, " (", e.what(), ")"));
            }
        }

        if(::mkdir(m_pool_subdir.c_str(), S_IRWXU) != 0) {
            throw std::runtime_error(
                    logger::build_message("Error creating pool subdir: ", m_pool_subdir, " (", strerror(errno), ")"));
        }
        m_initialized = true;
    }

    file_region_list regions;
    m_dealloc_mutex.lock_shared();
    {
        boost::unique_lock<boost::shared_mutex> lock(m_alloc_mutex);
        // this will allocate any additional segments required
        fetch_storage(start_offset, size, regions);
    }
    update_size(start_offset+size);
    m_alloc_mutex.lock();
    m_attributes.st_ctime = time(NULL);
    m_alloc_mutex.unlock();
    m_dealloc_mutex.unlock_shared();
    #if defined(__EFS_DEBUG__) && defined(__PRINT_TREE__)
    print_tree(m_segments);
    #endif
    return 0;
}


void file::change_type(file::type type){
	m_type = type;
}

void file::truncate(off_t end_offset) {

    if ((unsigned)end_offset > (unsigned)size() ) { 
        allocate( 0, end_offset);
    }
    else {
	
      m_dealloc_mutex.lock();
      auto rl = lock_range(end_offset, size(), efsng::operation::write);
      // Traverse tree, update m_bytes of the last active segment, deallocate pmemblocks, set gap as 1

         ssize_t size = 0;
         bool cut = false;
         for(auto it = m_segments.begin(); it != m_segments.end(); ++it) {

            const auto& sptr = it->second;

            if(sptr != nullptr) {
                //std::cerr << *sptr << "\n";
                size += sptr->m_bytes ? sptr->m_bytes : sptr->m_size;
                if (size > end_offset){
                   
                    if (cut == false) {
                           // std::cout << " CUT HERE " << size - end_offset << std::endl;
                            sptr->m_bytes = (size - end_offset) ; // It is not used
                            cut = true;
                        }
                        else {
                           // std::cout << " DROP SEGMENT " << size - end_offset << std::endl;
                            pmem_unmap(sptr->m_pool.m_data, sptr->m_pool.m_length);
                            sptr->m_pool.m_data = NULL;
                            sptr->m_is_gap = 1;
                        }
                }
            }
            
        }

        //We cannot call update size, as it is a truncate
        m_alloc_mutex.lock();
        m_used_offset = end_offset;
        m_attributes.st_size = end_offset;
        m_attributes.st_blocks = end_offset/512;
        m_attributes.st_ctime = time(NULL);
        m_alloc_mutex.unlock();
	unlock_range(rl);
        m_dealloc_mutex.unlock();
    }
}


} // namespace nvml
} // namespace efsng
