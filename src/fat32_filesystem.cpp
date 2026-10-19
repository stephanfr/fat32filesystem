// Copyright 2023 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <stdint.h>

#include "filesystem/fat32_filesystem.h"
#include "filesystem/fat32_partition.h"

namespace filesystems::fat32
{
    //
    //  FAT32Filesystem methods
    //

    PointerResult<FilesystemResultCodes, FAT32Filesystem> FAT32Filesystem::Mount(bool permanent,
                                                                                 const char *name,
                                                                                 const char *alias,
                                                                                 bool boot,
                                                                                 BlockIODevice &io_device,
                                                                                 const MassStoragePartition &partition)
    {
        using Result = PointerResult<FilesystemResultCodes, FAT32Filesystem>;

        LogEntryAndExit("Entering\n");

        //  Insure this is a FAT32 filesystem - use the MBR partition type

        if (partition.Type() != FilesystemTypes::FAT32)
        {
            LogDebug1("Filesystem is not FAT32\n");
            return Result::Failure(FilesystemResultCodes::FAT32_NOT_A_FAT32_FILESYSTEM);
        }

        //  Mount the block IO adapter

        auto *opaque_data = (FAT32PartitionOpaqueData *)(partition.GetOpaqueDataBlock());
        auto adapter = FAT32BlockIOAdapter::Mount(io_device, opaque_data->first_sector_, opaque_data->num_sectors_);

        ReturnOnFailure(adapter);

        //  Return the filesystem

        minstd::unique_ptr<FAT32Filesystem> new_filesystem;

        if (permanent)
        {
            new_filesystem = make_static_unique<FAT32Filesystem>(permanent, name, alias, boot, *adapter, partition.Name());
        }
        else
        {
            new_filesystem = make_dynamic_unique<FAT32Filesystem>(permanent, name, alias, boot, *adapter, partition.Name());
        }

        return Result::Success(minstd::move(new_filesystem));
    }

    PointerResult<FilesystemResultCodes, FilesystemDirectory> FAT32Filesystem::GetRootDirectory()
    {
        using Result = PointerResult<FilesystemResultCodes, FilesystemDirectory>;

        LogEntryAndExit("Entering\n");

        minstd::unique_ptr<FilesystemDirectory> directory(FAT32Directory::AsFilesystemDirectory(Id(),
                                                                                                "/",
                                                                                                FAT32DirectoryEntryAddress(),
                                                                                                block_io_adapter_.RootDirectoryCluster(),
                                                                                                FAT32Compact8Dot3Filename("/", "")));

        return Result::Success(minstd::move(directory));
    }

    PointerResult<FilesystemResultCodes, FilesystemDirectory> FAT32Filesystem::GetDirectory(const minstd::string &path)
    {
        using Result = PointerResult<FilesystemResultCodes, FilesystemDirectory>;

        LogEntryAndExit("Entering with path: %s\n", path.c_str());

        //  Lock the filesystem to ensure thread safety

        minstd::lock_guard guard(lock_);

        //  Parse the path, return immediately if it is not parseable

        auto parsed_path = FilesystemPath::ParsePathString(path);

        ReturnOnFailure(parsed_path);

        //  Return immediately if the root directory was requested

        if (parsed_path->IsRoot())
        {
            LogDebug1("Is Root Directory with First Cluster: %u\n", block_io_adapter_.RootDirectoryCluster());

            minstd::unique_ptr<FilesystemDirectory> directory(FAT32Directory::AsFilesystemDirectory(Id(),
                                                                                                    path,
                                                                                                    FAT32DirectoryEntryAddress(),
                                                                                                    block_io_adapter_.RootDirectoryCluster(),
                                                                                                    FAT32Compact8Dot3Filename("/", "")));

            return Result::Success(minstd::move(directory));
        }

        //  Resolve the path one directory at a time

        auto resolved = ResolveDirectory(**parsed_path);

        ReturnOnFailure(resolved);

        //  Create the pointer to the directory and return it

        minstd::unique_ptr<FilesystemDirectory> directory(FAT32Directory::AsFilesystemDirectory(Id(),
                                                                                                path,
                                                                                                resolved->EntryAddress(),
                                                                                                resolved->FirstClusterId(),
                                                                                                resolved->CompactName()));

        return Result::Success(minstd::move(directory));
    }

    ValueResult<FilesystemResultCodes, FAT32DirectoryCacheEntry> FAT32Filesystem::ResolveDirectory(const FilesystemPath &path)
    {
        using Result = ValueResult<FilesystemResultCodes, FAT32DirectoryCacheEntry>;

        LogEntryAndExit("Entering with directory: %s\n", path.FullPath().c_str());

        if (path.IsRoot())
        {
            return Result::Failure(FilesystemResultCodes::DIRECTORY_NOT_FOUND);
        }

        //  Walk the path from the root, one name at a time.  Each step asks the cache what this name in
        //      this directory is, and reads the directory from disk only when the cache does not know.

        const FAT32ClusterIndex root_cluster = block_io_adapter_.RootDirectoryCluster();

        FAT32ClusterIndex parent = root_cluster;
        FAT32DirectoryCacheEntry resolved;

        for (auto itr = path.begin(); itr != path.end(); itr++)
        {
            const char *name = *itr;

            auto cached = directory_cache_.Find(parent, name);

            if (cached.has_value())
            {
                resolved = *cached;
            }
            else
            {
                FAT32DirectoryCluster directory(Id(), block_io_adapter_, parent);

                auto entry = directory.FindDirectoryEntry(FilesystemDirectoryEntryType::DIRECTORY, name);

                ReturnOnFailure(entry);

                if (entry->end())
                {
                    LogDebug1("Could not find FAT32 subdirectory entry: %s\n", name);
                    return Result::Failure(FilesystemResultCodes::DIRECTORY_NOT_FOUND);
                }

                auto cluster_entry = entry->AsClusterEntry();

                ReturnOnFailure(cluster_entry);

                resolved = FAT32DirectoryCacheEntry(entry->AsEntryAddress(),
                                                    cluster_entry->FirstCluster(root_cluster),
                                                    cluster_entry->CompactName());

                directory_cache_.Add(parent, name, resolved);
            }

            parent = resolved.FirstClusterId();
        }

        return Result::Success(resolved);
    }
} // namespace filesystems::fat32
