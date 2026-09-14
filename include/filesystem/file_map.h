// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include "filesystem/filesystems.h"

#include <map>
#include "heaps.h"

#include "devices/log.h"

namespace filesystems
{

    class FileMap
    {
    public:
        FileMap() = default;

        ReferenceResult<FilesystemResultCodes, File> AddFile(minstd::unique_ptr<File> &&file)
        {
            using Result = ReferenceResult<FilesystemResultCodes, File>;

            auto path = file->AbsolutePath();

            if (!path.Successful())
            {
                return Result::Failure(path.ResultCode());
            }

            const bool new_open_is_exclusive = IsExclusive(file->Mode());

            for (auto itr = open_files_.begin(); itr != open_files_.end(); ++itr)
            {
                File &open_file = *(minstd::get<1>(*itr));

                auto open_path = open_file.AbsolutePath();

                if (!open_path.Successful() || !(*open_path == *path))
                {
                    continue;
                }

                if (new_open_is_exclusive || IsExclusive(open_file.Mode()))
                {
                    return Result::Failure(FilesystemResultCodes::FILE_ALREADY_OPENED_EXCLUSIVELY);
                }
            }

            //  Take the UUID BEFORE the move -- file is null afterwards, and the argument
            //      evaluation order of insert() is unspecified.

            const UUID id = file->ID();

            auto insert_result = open_files_.insert(id, minstd::move(file));

            if (minstd::get<1>(insert_result) == false)
            {
                return Result::Failure(FilesystemResultCodes::FILE_ALREADY_OPENED_EXCLUSIVELY);
            }

            return Result::Success(*(minstd::get<1>(*minstd::get<0>(insert_result))));
        }

        FilesystemResultCodes RemoveFile(const File &file)
        {
            const UUID id = file.ID();

            if (open_files_.erase(id) != 1)
            {
                LogError("File not found in file map.");

                return FilesystemResultCodes::FILE_NOT_OPEN;
            }

            return FilesystemResultCodes::SUCCESS;
        }

        bool IsFileOpen(const minstd::string &path)
        {
            for (auto itr = open_files_.begin(); itr != open_files_.end(); ++itr)
            {
                auto open_path = minstd::get<1>(*itr)->AbsolutePath();

                if (open_path.Successful() && (*open_path == path))
                {
                    return true;
                }
            }

            return false;
        }

        ReferenceResult<FilesystemResultCodes, File> GetFileByUUID(const UUID &uuid)
        {
            using Result = ReferenceResult<FilesystemResultCodes, File>;

            auto itr = open_files_.find(uuid);

            if (itr == open_files_.end())
            {
                return Result::Failure(FilesystemResultCodes::FILE_IS_CLOSED);
            }

            return Result::Success(*(minstd::get<1>(*itr)));
        }

    private:
        //  Anything beyond READ is a writer.

        static bool IsExclusive(FileModes mode)
        {
            return (static_cast<uint32_t>(mode) & ~static_cast<uint32_t>(FileModes::READ)) != 0;
        }

        //  One map, and it owns the files.

        using OpenFileMap = minstd::map<UUID, minstd::unique_ptr<File>>;
        using OpenFileMapAllocator = minstd::pmr::polymorphic_allocator<OpenFileMap::node_type>;

        OpenFileMapAllocator open_files_allocator_{&__os_dynamic_heap_resource};
        OpenFileMap open_files_{open_files_allocator_};
    };

    FileMap &GetFileMap();
} // namespace filesystems
