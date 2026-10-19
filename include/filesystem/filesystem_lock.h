// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "os_config.h"

#include <mutex>
#include <stdint.h>

//
//  FILESYSTEM_LOCKING selects whether the filesystem serialises access across tasks.
//
//      1 (default)  every volume, and the global FileMap, is guarded by a spin lock.  Required
//                   whenever more than one task - or core - can call into the filesystem.
//
//      0            the locks compile to nothing.  Only for builds where a single task ever
//                   touches the filesystem; GetCurrentTaskOwnerId() then needs no definition.
//
//  Set it in os_config.h or with -DFILESYSTEM_LOCKING=0.  It must be the same for every
//      translation unit that includes the filesystem headers.
//

#ifndef FILESYSTEM_LOCKING
#define FILESYSTEM_LOCKING 1
#endif

//  An ID unique to the calling task while it lives, and never 0.  Defined by whatever links the
//      filesystem: RPIBareMetalOS on the target, test/src/cpputest_main.cpp in the unit tests.
//      Only referenced when FILESYSTEM_LOCKING is 1.

uintptr_t GetCurrentTaskOwnerId() noexcept;

namespace filesystems
{
    //  Satisfies the same lock()/try_lock()/unlock() interface as the minstd mutexes, and does
    //      nothing.  Every call inlines away.

    class NullMutex
    {
    public:
        constexpr NullMutex() noexcept = default;

        NullMutex(const NullMutex &) = delete;
        NullMutex &operator=(const NullMutex &) = delete;

        void lock() noexcept {}

        bool try_lock() noexcept
        {
            return true;
        }

        void unlock() noexcept {}
    };

#if FILESYSTEM_LOCKING

    struct TaskOwnerPolicy
    {
        static uintptr_t current_owner() noexcept
        {
            return GetCurrentTaskOwnerId();
        }
    };

    //  One per mounted volume.  Recursive: public operations call one another (Append -> SeekEnd
    //      -> Seek, GetDirectory("..") -> FAT32Filesystem::GetDirectory), and VisitDirectory
    //      callbacks may call back in.

    using FilesystemMutex = minstd::recursive_spin_mutex<TaskOwnerPolicy>;

    //  The one global FileMap.  Its methods never call one another, so it need not be recursive.

    using FileMapMutex = minstd::spin_mutex;

#else

    using FilesystemMutex = NullMutex;
    using FileMapMutex = NullMutex;

#endif
} // namespace filesystems
