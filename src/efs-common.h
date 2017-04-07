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

#ifndef __EFS_COMMON_H__
#define __EFS_COMMON_H__

#include <cstdint>

namespace efsng {

using data_ptr_t = void *;

const uint64_t EFS_BLOCK_SIZE  = 0x000400000; // 4MiB
const uint64_t FUSE_BLOCK_SIZE = 0x000400000; // 4MiB

template <typename T>
inline T align(const T n, const T block_size) {
    return n & ~(block_size - 1);
}

template <typename T>
inline T xalign(const T n, const T block_size) {
    return align(n + block_size, block_size);
}

} // namespace efsng

#endif /* __EFS_COMMON_H__ */