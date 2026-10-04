// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "filesystem/filesystems.h"

#include <map>
#include "heaps.h"

#include "filesystem/filesystem_lock.h"

#include "devices/log.h"

namespace filesystems
{
    //  Identifies an open file by where it lives, not by how it was named: the filesystem it is on,
    //      and a filesystem-specific location for its directory entry.  Spelling, 8.3 aliases and
    //      renames of parent directories cannot change it, and equal paths on different
    //      filesystems do not collide.

    struct OpenFileIdentity
    {
        UUID filesystem_uuid;
        uint64_t entry_location;

        bool operator==(const OpenFileIdentity &other) const
        {
            return (filesystem_uuid == other.filesystem_uuid) && (entry_location == other.entry_location);
        }
    };

    class FileMap
    {
    public:
        FileMap() = default;

        ReferenceResult<FilesystemResultCodes, File> AddFile(minstd::unique_ptr<File> &&file, const OpenFileIdentity &identity)
        {
            using Result = ReferenceResult<FilesystemResultCodes, File>;

            minstd::lock_guard guard(lock_);

            const bool new_open_is_exclusive = IsExclusive(file->Mode());

            for (auto itr = records_.begin(); itr != records_.end(); ++itr)
            {
                const OpenFileRecord &record = minstd::get<1>(*itr);

                if (!(record.identity == identity))
                {
                    continue;
                }

                if (new_open_is_exclusive || IsExclusive(record.mode))
                {
                    return Result::Failure(FilesystemResultCodes::FILE_ALREADY_OPENED_EXCLUSIVELY);
                }
            }

            //  Take the UUID and mode BEFORE the move -- file is null afterwards, and the argument
            //      evaluation order of insert() is unspecified.

            const UUID id = file->ID();
            const FileModes mode = file->Mode();

            auto insert_result = open_files_.insert(id, minstd::move(file));

            if (minstd::get<1>(insert_result) == false)
            {
                return Result::Failure(FilesystemResultCodes::FILE_ALREADY_OPENED_EXCLUSIVELY);
            }

            records_.insert(id, OpenFileRecord{identity, mode});

            return Result::Success(*(minstd::get<1>(*minstd::get<0>(insert_result))));
        }

        FilesystemResultCodes RemoveFile(const File &file)
        {
            minstd::lock_guard guard(lock_);

            const UUID id = file.ID();      //  before the erase below destroys the file

            records_.erase(id);

            if (open_files_.erase(id) != 1)
            {
                LogError("File not found in file map.");

                return FilesystemResultCodes::FILE_NOT_OPEN;
            }

            return FilesystemResultCodes::SUCCESS;
        }

        bool IsFileOpen(const OpenFileIdentity &identity)
        {
            minstd::lock_guard guard(lock_);

            for (auto itr = records_.begin(); itr != records_.end(); ++itr)
            {
                if (minstd::get<1>(*itr).identity == identity)
                {
                    return true;
                }
            }

            return false;
        }

        //  The reference outlives the lock.  That is safe because only the one FileWrapper holding
        //      this UUID uses - and closes - the file.

        ReferenceResult<FilesystemResultCodes, File> GetFileByUUID(const UUID &uuid)
        {
            using Result = ReferenceResult<FilesystemResultCodes, File>;

            minstd::lock_guard guard(lock_);

            auto itr = open_files_.find(uuid);

            if (itr == open_files_.end())
            {
                return Result::Failure(FilesystemResultCodes::FILE_IS_CLOSED);
            }

            return Result::Success(*(minstd::get<1>(*itr)));
        }

#ifdef INCLUDE_TEST_HELPERS
        void Clear()
        {
            minstd::lock_guard guard(lock_);

            records_.clear();
            open_files_.clear();
        }
#endif

    private:
        struct OpenFileRecord
        {
            OpenFileIdentity identity;
            FileModes mode;
        };

        //  Anything beyond READ is a writer.

        static bool IsExclusive(FileModes mode)
        {
            return (static_cast<uint32_t>(mode) & ~static_cast<uint32_t>(FileModes::READ)) != 0;
        }

        //  One global map shared by every filesystem and task.  Its methods never call each other,
        //      so a plain spin lock suffices.  Callers holding a filesystem lock take this one second.

        FileMapMutex lock_;

        //  The files themselves, owned here, and what is known about each, both keyed by file UUID.

        using OpenFileMap = minstd::map<UUID, minstd::unique_ptr<File>>;
        using OpenFileMapAllocator = minstd::pmr::polymorphic_allocator<OpenFileMap::node_type>;

        using OpenFileRecordMap = minstd::map<UUID, OpenFileRecord>;
        using OpenFileRecordMapAllocator = minstd::pmr::polymorphic_allocator<OpenFileRecordMap::node_type>;

        OpenFileMapAllocator open_files_allocator_{&__os_dynamic_heap_resource};
        OpenFileMap open_files_{open_files_allocator_};

        OpenFileRecordMapAllocator records_allocator_{&__os_dynamic_heap_resource};
        OpenFileRecordMap records_{records_allocator_};
    };

    FileMap &GetFileMap();
} // namespace filesystems
