// Copyright 2023 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "os_config.h"

#include <map>
#include <minimalcstdlib.h>

#include "heaps.h"

#include "filesystem/filesystems.h"
#include "filesystem/master_boot_record.h"

#include "filesystem/fat32_blockio_adapter.h"
#include "filesystem/fat32_directory.h"
#include "filesystem/fat32_directory_cache.h"
#include "filesystem/fat32_directory_cluster.h"
#include "filesystem/fat32_file.h"
#include "filesystem/filesystem_lock.h"

namespace filesystems::fat32
{
    class FAT32Filesystem;

    class FAT32FilesystemStatistics
    {
    public:
        uint64_t DirectoryCacheHits() const
        {
            return directory_cache_.Hits();
        }

        uint64_t DirectoryCacheMisses() const
        {
            return directory_cache_.Misses();
        }

    private:
        friend class FAT32Filesystem;

        FAT32FilesystemStatistics(const FAT32DirectoryCache &directory_cache)
            : directory_cache_(directory_cache)
        {
        }

        const FAT32DirectoryCache &directory_cache_;
    };

    class FAT32Filesystem : public Filesystem
    {
    public:
        static PointerResult<FilesystemResultCodes, FAT32Filesystem> Mount(bool permanent,
                                                                           const char *name,
                                                                           const char *alias,
                                                                           bool boot,
                                                                           BlockIODevice &io_device,
                                                                           const MassStoragePartition &partition);

        FAT32Filesystem(bool permanent,
                        const char *name,
                        const char *alias,
                        bool boot,
                        FAT32BlockIOAdapter block_io_adapter,
                        const minstd::string &volume_label)
            : Filesystem(permanent, name, alias, boot),
              volume_label_(volume_label),
              block_io_adapter_(block_io_adapter),
              statistics_(directory_cache_)
        {
        }

        FAT32Filesystem() = delete;

        virtual ~FAT32Filesystem() {}

        const minstd::string &VolumeLabel() const noexcept
        {
            return volume_label_;
        }

        FAT32FilesystemStatistics Statistics() const noexcept
        {
            return statistics_;
        }

        FAT32BlockIOAdapter &BlockIOAdapter()
        {
            return block_io_adapter_;
        }

        FAT32DirectoryCache &DirectoryCache()
        {
            return directory_cache_;
        }

        FilesystemMutex &FilesystemLock() noexcept
        {
            return lock_;
        }

        uint64_t DirectoryGeneration() const noexcept
        {
            return directory_generation_;
        }

        void InvalidateDirectoryHandles() noexcept
        {
            directory_generation_++;
        }

        //  A held lock on a mounted volume, from Lock().  Releases the lock when it goes out of scope.
        //      Tests false if no such filesystem was mounted, in which case it holds nothing.

        class Locked
        {
        public:
            ~Locked()
            {
                if (filesystem_ != nullptr)
                {
                    filesystem_->lock_.unlock();
                }
            }

            Locked(const Locked &) = delete;
            Locked(Locked &&) = delete;
            Locked &operator=(const Locked &) = delete;
            Locked &operator=(Locked &&) = delete;

            explicit operator bool() const noexcept
            {
                return filesystem_ != nullptr;
            }

            FAT32Filesystem &operator*() const noexcept
            {
                return *filesystem_;
            }

            FAT32Filesystem *operator->() const noexcept
            {
                return filesystem_;
            }

        private:
            friend class FAT32Filesystem;

            explicit Locked(FAT32Filesystem *filesystem) noexcept
                : filesystem_(filesystem)
            {
                if (filesystem_ != nullptr)
                {
                    filesystem_->lock_.lock();
                }
            }

            FAT32Filesystem *const filesystem_;
        };

        //  Finds a mounted FAT32 filesystem by UUID and locks it, in one step.  Every public directory
        //      and file operation starts here, and holds the result until it returns.  The lock is
        //      recursive, so an operation that calls another public operation simply nests.

        static Locked Lock(const UUID &filesystem_uuid)
        {
            auto entity = GetOSEntityRegistry().GetEntityById(filesystem_uuid);

            if (!entity.Successful())
            {
                return Locked(nullptr);
            }

            FAT32Filesystem &filesystem = entity;

            return Locked(&filesystem);
        }

        PointerResult<FilesystemResultCodes, FilesystemDirectory> GetRootDirectory() override;

        PointerResult<FilesystemResultCodes, FilesystemDirectory> GetDirectory(const minstd::string &path) override;

    private:
        const minstd::fixed_string<MAX_FILENAME_LENGTH> volume_label_;

        FAT32BlockIOAdapter block_io_adapter_;
        FAT32DirectoryCache directory_cache_{DEFAULT_DIRECTORY_CACHE_SIZE};

        FAT32FilesystemStatistics statistics_;

        FilesystemMutex lock_;

        uint64_t directory_generation_ = 1;

        ValueResult<FilesystemResultCodes, FAT32DirectoryCacheEntry> ResolveDirectory(const FilesystemPath &path);
    };
} // namespace filesystems::fat32
