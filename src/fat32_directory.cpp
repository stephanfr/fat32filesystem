// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <stdint.h>

#include "filesystem/fat32_file.h"
#include "filesystem/fat32_filesystem.h"
#include "filesystem/file_wrapper.h"

namespace filesystems::fat32
{
    //
    //  FAT32Directory
    //

    FilesystemResultCodes FAT32Directory::VisitDirectory(FilesystemDirectoryVisitorCallback callback) const
    {
        using Result = FilesystemResultCodes;

        LogEntryAndExit("Entering with First Cluster: %u\n", first_cluster_);

        //  Get the filesystem entity

        auto locked_filesystem = FAT32Filesystem::Lock(FilesystemUUID());

        if (!locked_filesystem)
        {
            return FilesystemResultCodes::FILESYSTEM_DOES_NOT_EXIST;
        }

        FAT32Filesystem &filesystem = *locked_filesystem;
        ReturnOnCallFailure(ValidateHandle(filesystem));

        //  Create a directory cluster object

        FAT32DirectoryCluster current_directory = FAT32DirectoryCluster(filesystem.Id(),
                                                                        filesystem.BlockIOAdapter(),
                                                                        first_cluster_);

        //  Iterate over the entries in the directory

        FAT32DirectoryCluster::directory_entry_const_iterator itr = current_directory.directory_entry_iterator_begin();

        ReturnOnCallFailure(itr++);

        while (!itr.end())
        {
            auto current_entry = itr.AsDirectoryEntry();

            ReturnOnFailure(current_entry);

            if (callback(*current_entry) == FilesystemDirectoryVisitorCallbackStatus::FINISHED)
            {
                break;
            }

            ReturnOnCallFailure(itr++);
        }

        //  Return success

        return FilesystemResultCodes::SUCCESS;
    }

    ValueResult<FilesystemResultCodes, FilesystemDirectoryEntry> FAT32Directory::GetEntry(FAT32BlockIOAdapter &block_io_adapter, const minstd::string &entry_name, FilesystemDirectoryEntryType type) const
    {
        using Result = ValueResult<FilesystemResultCodes, FilesystemDirectoryEntry>;

        //  Create a directory cluster object

        FAT32DirectoryCluster current_directory = FAT32DirectoryCluster(FilesystemUUID(),
                                                                        block_io_adapter,
                                                                        first_cluster_);

        auto entry = current_directory.FindDirectoryEntry(type, entry_name.c_str());

        ReturnOnFailure(entry, LogError("Error Finding Entry: %s\n", entry_name.c_str()));

        if (!entry->end())
        {
            auto directory_entry = entry->AsDirectoryEntry();

            ReturnOnFailure(directory_entry);

            //  Return the directory entry

            return directory_entry;
        }

        //  If we end up down here, we never found the entry.
        //      Return a type-specific error code.

        if (type == FilesystemDirectoryEntryType::DIRECTORY)
        {
            return Result::Failure(FilesystemResultCodes::DIRECTORY_NOT_FOUND);
        }
        else if (type == FilesystemDirectoryEntryType::VOLUME_INFORMATION)
        {
            return Result::Failure(FilesystemResultCodes::VOLUME_INFORMATION_NOT_FOUND);
        }

        return Result::Failure(FilesystemResultCodes::FILE_NOT_FOUND);
    }

    FilesystemResultCodes FAT32Directory::ValidateHandle(FAT32Filesystem &filesystem) const
    {
        //  Nothing that could move or remove a directory has happened since this handle was last checked.

        if (validated_at_generation_ == filesystem.DirectoryGeneration())
        {
            return FilesystemResultCodes::SUCCESS;
        }

        //  The root can be neither renamed nor removed.  Anything else must still be found at its
        //      path, as the same entry with the same chain - otherwise it was renamed, removed, or
        //      removed and replaced.

        if (!IsRoot())
        {
            auto current = filesystem.GetDirectory(path_);

            if (!current.Successful())
            {
                return (current.ResultCode() == FilesystemResultCodes::DIRECTORY_NOT_FOUND) ? FilesystemResultCodes::DIRECTORY_HANDLE_IS_STALE
                                                                                            : current.ResultCode();
            }

            const FAT32Directory &now = static_cast<const FAT32Directory &>(**current);

            if ((now.FirstCluster() != first_cluster_) ||
                (now.EntryAddress().Cluster() != entry_address_.Cluster()) ||
                (now.EntryAddress().Index() != entry_address_.Index()))
            {
                return FilesystemResultCodes::DIRECTORY_HANDLE_IS_STALE;
            }
        }

        validated_at_generation_ = filesystem.DirectoryGeneration();

        return FilesystemResultCodes::SUCCESS;
    }

    inline PointerResult<FilesystemResultCodes, FilesystemDirectory> FAT32Directory::GetDotEntry() const
    {
        using Result = PointerResult<FilesystemResultCodes, FilesystemDirectory>;

        minstd::unique_ptr<FilesystemDirectory> directory(AsFilesystemDirectory(FilesystemUUID(),
                                                                                path_,
                                                                                entry_address_,
                                                                                first_cluster_,
                                                                                FAT32Compact8Dot3Filename(".", "")));

        return Result::Success(minstd::move(directory));
    }

    PointerResult<FilesystemResultCodes, FilesystemDirectory> FAT32Directory::GetDotDotEntry(FAT32Filesystem &filesystem) const
    {
        using Result = PointerResult<FilesystemResultCodes, FilesystemDirectory>;

        //  There is no dot dot entry for the root directory

        if (IsRoot())
        {
            return GetDotEntry();
        }

        //  Get the parent path from the current absolute path.

        minstd::fixed_string<MAX_FILESYSTEM_PATH_LENGTH> parent_path;

        size_t last_slash = path_.find_last_of('/');

        if (last_slash != minstd::fixed_string<MAX_FILESYSTEM_PATH_LENGTH>::npos)
        {
            if (last_slash != 0)
            {
                path_.substr(parent_path, 0, last_slash);
            }
            else
            {
                parent_path = "/";
            }
        }
        else
        {
            //  We should never get here.

            return Result::Failure(FilesystemResultCodes::ILLEGAL_PATH);
        }

        //  The parent's own directory entry lives in the grandparent.  Resolve it by path: the
        //      '..' slot in this directory is not that entry, and RemoveDirectory() on a handle
        //      carrying its address deletes the wrong slot.

        return filesystem.GetDirectory(parent_path);
    }

    PointerResult<FilesystemResultCodes, FilesystemDirectory> FAT32Directory::GetDirectory(const minstd::string &directory_name)
    {
        using Result = PointerResult<FilesystemResultCodes, FilesystemDirectory>;

        LogEntryAndExit("Entering with directory name: %s\n", directory_name.c_str());

        //  Get the filesystem entity

        auto locked_filesystem = FAT32Filesystem::Lock(FilesystemUUID());

        if (!locked_filesystem)
        {
            return Result::Failure(FilesystemResultCodes::FILESYSTEM_DOES_NOT_EXIST);
        }

        FAT32Filesystem &filesystem = *locked_filesystem;
        ReturnOnCallFailure(ValidateHandle(filesystem));

        //  Get the block io adapter from the filesystem

        FAT32BlockIOAdapter &block_io_adapter = filesystem.BlockIOAdapter();

        //  Two special cases: '.' and '..'
        //      For dot, simply return this directory

        if (directory_name == ".")
        {
            return GetDotEntry();
        }

        //  For dot-dot return the parent directory

        if (directory_name == "..")
        {
            return GetDotDotEntry(filesystem);
        }

        //  We need the full path - but there is a special case for the root directory, we do not add a forward slash.

        minstd::fixed_string<MAX_FILESYSTEM_PATH_LENGTH> directory_absolute_path(path_);

        AppendToPath(directory_absolute_path, directory_name);

        //  Check the cache for the directory

        auto cache_entry = filesystem.DirectoryCache().FindEntry(directory_absolute_path);

        if (cache_entry.has_value())
        {
            //  Found the directory in the cache

            minstd::unique_ptr<FilesystemDirectory> directory(AsFilesystemDirectory(filesystem.Id(),
                                                                                    directory_absolute_path,
                                                                                    cache_entry.value().get().EntryAddress(),
                                                                                    cache_entry.value().get().FirstClusterId(),
                                                                                    cache_entry.value().get().CompactName()));

            return Result::Success(minstd::move(directory));
        }

        //  We need to search the directory cluster for the directory

        auto directory_entry = GetEntry(block_io_adapter, directory_name, FilesystemDirectoryEntryType::DIRECTORY);

        ReturnOnFailure(directory_entry);

        //  Found the directory entry, so add it to the cache

        filesystem.DirectoryCache().AddEntry(FAT32DirectoryCacheEntryType::DIRECTORY,
                                             GetOpaqueData(*directory_entry).directory_entry_address_,
                                             GetOpaqueData(*directory_entry).FirstCluster(),
                                             GetOpaqueData(*directory_entry).directory_entry_.CompactName(),
                                             directory_absolute_path);

        //  Create the directory object and return it

        minstd::unique_ptr<FilesystemDirectory> directory(AsFilesystemDirectory(filesystem.Id(),
                                                                                directory_absolute_path,
                                                                                GetOpaqueData(*directory_entry).directory_entry_address_,
                                                                                GetOpaqueData(*directory_entry).FirstCluster(),
                                                                                GetOpaqueData(*directory_entry).directory_entry_.CompactName()));

        return Result::Success(minstd::move(directory));
    }

    PointerResult<FilesystemResultCodes, FilesystemDirectory> FAT32Directory::CreateDirectory(const minstd::string &new_directory_name)
    {
        using Result = PointerResult<FilesystemResultCodes, FilesystemDirectory>;

        LogEntryAndExit("Entering with directory name: %s\n", new_directory_name.c_str());

        //  Get the filesystem entity

        auto locked_filesystem = FAT32Filesystem::Lock(FilesystemUUID());

        if (!locked_filesystem)
        {
            return Result::Failure(FilesystemResultCodes::FILESYSTEM_DOES_NOT_EXIST);
        }

        FAT32Filesystem &filesystem = *locked_filesystem;
        ReturnOnCallFailure(ValidateHandle(filesystem));

        //  Get the block io adpater from the filesystem

        FAT32BlockIOAdapter &block_io_adapter = filesystem.BlockIOAdapter();

        //  Create a directory cluster object

        LogDebug1("Creating Subdirectory with Parent First Cluster: %u\n", first_cluster_);

        FAT32DirectoryCluster directory_cluster = FAT32DirectoryCluster(filesystem.Id(),
                                                                        block_io_adapter,
                                                                        first_cluster_);

        //  We need an empty cluster to create the new directory

        auto new_directory_first_cluster = block_io_adapter.FindNextEmptyCluster();

        ReturnOnFailure(new_directory_first_cluster);

        ReturnOnCallFailure(directory_cluster.WriteEmptyDirectoryCluster(*new_directory_first_cluster, first_cluster_));

        //  Update the FAT Table to indicate we have only the single entry

        if (block_io_adapter.UpdateFATTableEntry(*new_directory_first_cluster, FAT32EntryAllocatedAndEndOfFile) != FilesystemResultCodes::SUCCESS)
        {
            return Result::Failure(FilesystemResultCodes::FAT32_UNABLE_TO_WRITE_FAT_TABLE_SECTOR);
        }

        //  Create the directory entry in the parent directory

        auto new_directory_entry = directory_cluster.CreateEntry(new_directory_name,
                                                                 FAT32DirectoryEntryAttributeFlags::FAT32DirectoryEntryAttributeDirectory,
                                                                 FAT32TimeHundredths(0),
                                                                 FAT32Time(0, 0, 0),
                                                                 FAT32Date(1980, 1, 1),
                                                                 FAT32Date(1980, 1, 1),
                                                                 *new_directory_first_cluster,
                                                                 FAT32Time(0, 0, 0),
                                                                 FAT32Date(1980, 1, 1),
                                                                 0);

        //  If anything went wrong, free the cluster allocated above and return the error

        if (new_directory_entry.Failed())
        {
            if (block_io_adapter.UpdateFATTableEntry(*new_directory_first_cluster, FAT32EntryFree) != FilesystemResultCodes::SUCCESS)
            {
                LogError("Unable to free cluster after failed directory entry creation\n");
            }

            return Result::Failure(new_directory_entry.ResultCode());
        }

        //  Create the directory object and return it

        minstd::fixed_string<MAX_FILESYSTEM_PATH_LENGTH> path(path_);

        AppendToPath(path, new_directory_name);

        minstd::unique_ptr<FilesystemDirectory> new_directory(AsFilesystemDirectory(filesystem.Id(),
                                                                                    path,
                                                                                    GetOpaqueData(*new_directory_entry).directory_entry_address_,
                                                                                    *new_directory_first_cluster,
                                                                                    GetOpaqueData(*new_directory_entry).directory_entry_.CompactName()));

        //  Return the new directory

        return Result::Success(minstd::move(new_directory));
    }

    FilesystemResultCodes FAT32Directory::RemoveDirectory()
    {
        using Result = FilesystemResultCodes;

        LogEntryAndExit("Entering with directory name: %s\n", path_.c_str());

        //  The root directory cannot be removed

        if (IsRoot())
        {
            return FilesystemResultCodes::ROOT_DIRECTORY_CANNOT_BE_REMOVED;
        }

        //  Get the filesystem entity

        auto locked_filesystem = FAT32Filesystem::Lock(FilesystemUUID());

        if (!locked_filesystem)
        {
            return FilesystemResultCodes::FILESYSTEM_DOES_NOT_EXIST;
        }

        FAT32Filesystem &filesystem = *locked_filesystem;
        ReturnOnCallFailure(ValidateHandle(filesystem));

        //  Get the block io adapter from the filesystem

        FAT32BlockIOAdapter &block_io_adapter = filesystem.BlockIOAdapter();

        //  Insure the directory still exists, if not return a directory not found error

        FAT32DirectoryCluster directory_cluster = FAT32DirectoryCluster(filesystem.Id(),
                                                                block_io_adapter,
                                                                FAT32ClusterIndex(0));

        auto cluster_entry = directory_cluster.GetClusterEntry(entry_address_);

        ReturnOnFailure(cluster_entry);

        //  The slot may have been reused by a different directory since this handle was
        //      opened.  The first cluster is what identifies *this* directory.

        if (cluster_entry->IsUnused() || !cluster_entry->IsDirectoryEntry() ||
            (cluster_entry->FirstCluster(block_io_adapter.RootDirectoryCluster()) != first_cluster_))
        {
            return FilesystemResultCodes::DIRECTORY_NOT_FOUND;
        }

        //  A directory must be empty before it may be removed.  Releasing the chain of a
        //      populated directory strands every descendant's clusters - they stay marked
        //      allocated in the FAT with nothing referencing them.

        {
            FAT32DirectoryCluster contents(filesystem.Id(), block_io_adapter, first_cluster_);

            auto itr = contents.directory_entry_iterator_begin();

            ReturnOnCallFailure(itr++);

            while (!itr.end())
            {
                auto entry = itr.AsClusterEntry();

                ReturnOnFailure(entry);

                minstd::fixed_string<MAX_FILENAME_LENGTH> entry_name;

                entry->Compact8Dot3Filename(entry_name);

                if ((entry_name != ".") && (entry_name != ".."))
                {
                    return FilesystemResultCodes::DIRECTORY_NOT_EMPTY;
                }

                ReturnOnCallFailure(itr++);
            }
        }

        //  The entry lives in the parent directory, and RemoveEntry() walks backwards through
        //      the parent's chain to clear the long filename entries.  FAT32Directory does not
        //      hold its parent, but our own '..' entry (always index 1) records the parent's
        //      first cluster - with 0 meaning root, which FirstCluster() maps for us.

        auto dot_dot_entry = directory_cluster.GetClusterEntry(FAT32DirectoryEntryAddress(first_cluster_, 1));

        ReturnOnFailure(dot_dot_entry);

        const FAT32ClusterIndex parent_first_cluster = dot_dot_entry->FirstCluster(block_io_adapter.RootDirectoryCluster());

        FAT32DirectoryCluster parent_directory_cluster(filesystem.Id(), block_io_adapter, parent_first_cluster);

        //  Remove any entry from the cache first

        filesystem.DirectoryCache().RemoveEntry(first_cluster_);
        filesystem.InvalidateDirectoryHandles();

        //  Remove the directory cluster entry

        ReturnOnCallFailure(parent_directory_cluster.RemoveEntry(entry_address_));

        //  Release the clusters for the directory

        ReturnOnCallFailure(block_io_adapter.ReleaseChain(cluster_entry->FirstCluster(block_io_adapter.RootDirectoryCluster())));

        //  Finished with Success

        return FilesystemResultCodes::SUCCESS;
    }

    PointerResult<FilesystemResultCodes, File> FAT32Directory::OpenFile(const minstd::string &filename, FileModes mode)
    {
        using Result = PointerResult<FilesystemResultCodes, File>;

        LogEntryAndExit("Entering with filename: %s\n", filename.c_str());

        //  Get the filesystem entity

        auto locked_filesystem = FAT32Filesystem::Lock(FilesystemUUID());

        if (!locked_filesystem)
        {
            return Result::Failure(FilesystemResultCodes::FILESYSTEM_DOES_NOT_EXIST);
        }

        FAT32Filesystem &filesystem = *locked_filesystem;
        ReturnOnCallFailure(ValidateHandle(filesystem));

        //  Get the block io adapter from the filesystem

        FAT32BlockIOAdapter &block_io_adapter = filesystem.BlockIOAdapter();

        //  If the file already exists, then open it

        auto file_entry = GetEntry(block_io_adapter, filename, FilesystemDirectoryEntryType::FILE);

        if (file_entry.Successful())
        {
            //  File exists, so open it.  Get the full path first and use the on-disk name
            //      so we don't run into problems with different spellings (long, short, etc.) of the filename.

            minstd::dynamic_string<MAX_FILESYSTEM_PATH_LENGTH> path(path_, __dynamic_string_allocator);
            AppendToPath(path, file_entry->Name());

            minstd::unique_ptr<File> file(static_cast<File *>(make_dynamic_unique<FAT32File>(FilesystemUUID(), *file_entry, path, mode, 0, 0).release()), __os_dynamic_heap_resource);

            auto file_ref = GetFileMap().AddFile(minstd::move(file), FAT32OpenFileIdentity(FilesystemUUID(), GetOpaqueData(*file_entry).directory_entry_address_));

            ReturnOnFailure(file_ref);

            minstd::unique_ptr<File> file_wrapper(static_cast<File *>(make_dynamic_unique<FileWrapper>(minstd::move(*file_ref)).release()), __os_dynamic_heap_resource);

            return Result::Success(minstd::move(file_wrapper));
        }

        //  File does not exist, so we need to create it.
        //      First insure that there is no other error than the file does not exist.

        if (file_entry.ResultCode() != FilesystemResultCodes::FILE_NOT_FOUND)
        {
            LogDebug1("Could Not Find Directory Entry for file: %s\n", filename.c_str());
            return Result::Failure(file_entry.ResultCode());
        }

        //  We have to have CREATE mode to create the file

        if (!HasFileMode(mode, FileModes::CREATE))
        {
            LogDebug1("Could Not Find Directory Entry for file - No Such File and Create Mode not Specified: %s\n", filename.c_str());
            return Result::Failure(FilesystemResultCodes::FILE_NOT_FOUND);
        }

        //  Create the file and return it

        auto fat32_file = CreateFile(block_io_adapter, filename, mode);

        ReturnOnFailure(fat32_file, LogDebug1("Error: %s attempting to create file named: %s\n", ErrorMessage(fat32_file.ResultCode()), filename.c_str()));

        const OpenFileIdentity identity = FAT32OpenFileIdentity(FilesystemUUID(), (*fat32_file)->DirectoryEntryAddress());

        minstd::unique_ptr<File> file(dynamic_cast<File *>(fat32_file.Value().release()), __os_dynamic_heap_resource);

        auto file_ref = GetFileMap().AddFile(minstd::move(file), identity);

        ReturnOnFailure(file_ref);

        minstd::unique_ptr<File> file_wrapper(static_cast<File *>(make_dynamic_unique<FileWrapper>(minstd::move(*file_ref)).release()), __os_dynamic_heap_resource);

        return Result::Success(minstd::move(file_wrapper));
    }

    PointerResult<FilesystemResultCodes, FAT32File> FAT32Directory::CreateFile(FAT32BlockIOAdapter &block_io_adapter, const minstd::string &filename, FileModes mode)
    {
        using Result = PointerResult<FilesystemResultCodes, FAT32File>;

        LogEntryAndExit("Entering with filename: %s\n", filename.c_str());

        //  Create a directory cluster object

        FAT32DirectoryCluster directory_cluster(FilesystemUUID(),
                                                block_io_adapter,
                                                FirstCluster());

         auto new_file_directory_entry = directory_cluster.CreateEntry(filename,
                                                                      FAT32DirectoryEntryAttributeFlags::FAT32DirectoryEntryAttributeArchive,
                                                                      FAT32TimeHundredths(0),
                                                                      FAT32Time(0, 0, 0),
                                                                      FAT32Date(1980, 1, 1),
                                                                      FAT32Date(1980, 1, 1),
                                                                      FAT32ClusterIndex(0),
                                                                      FAT32Time(0, 0, 0),
                                                                      FAT32Date(1980, 1, 1),
                                                                      0);

        ReturnOnFailure(new_file_directory_entry);

        minstd::dynamic_string<MAX_FILESYSTEM_PATH_LENGTH> path(path_, __dynamic_string_allocator);
        AppendToPath(path, new_file_directory_entry->Name());

        minstd::unique_ptr<FAT32File> file(make_dynamic_unique<FAT32File>(FilesystemUUID(), *new_file_directory_entry, path, mode, 0, 0).release(), __os_dynamic_heap_resource);

        return Result::Success(minstd::move(file));
    }

    FilesystemResultCodes FAT32Directory::SetDirectoryEntryFirstCluster(FAT32BlockIOAdapter &block_io_adapter, const FAT32DirectoryEntryAddress &address, FAT32ClusterIndex first_cluster)
    {
        if (!IsEntryIndexInCluster(block_io_adapter, address))
        {
            return FilesystemResultCodes::FAT32_CURRENT_DIRECTORY_ENTRY_IS_INVALID;
        }

        minstd::heap_buffer<uint8_t> cluster_buffer(__os_dynamic_heap_resource, block_io_adapter.BytesPerCluster());
        uint8_t *block_buffer = cluster_buffer.data();

        //  Read the directory block

        auto read_block_result = block_io_adapter.ReadCluster(address.Cluster(), block_buffer);

        if (read_block_result != BlockIOResultCodes::SUCCESS)
        {
            return FilesystemResultCodes::FAT32_DEVICE_READ_ERROR;
        }

        FAT32DirectoryClusterEntry &entry = ((FAT32DirectoryClusterEntry *)block_buffer)[address.Index()];

        entry.SetFirstCluster(first_cluster);

        auto write_block_result = block_io_adapter.WriteCluster(address.Cluster(), block_buffer);

        if (write_block_result != BlockIOResultCodes::SUCCESS)
        {
            return FilesystemResultCodes::FAT32_DEVICE_WRITE_ERROR;
        }

        return FilesystemResultCodes::SUCCESS;
    }

    FilesystemResultCodes FAT32Directory::UpdateDirectoryEntrySize(FAT32BlockIOAdapter &block_io_adapter, const FAT32DirectoryEntryAddress &address, uint32_t new_size)
    {
        if (!IsEntryIndexInCluster(block_io_adapter, address))
        {
            return FilesystemResultCodes::FAT32_CURRENT_DIRECTORY_ENTRY_IS_INVALID;
        }
        
        minstd::heap_buffer<uint8_t> cluster_buffer(__os_dynamic_heap_resource, block_io_adapter.BytesPerCluster());
        uint8_t *block_buffer = cluster_buffer.data();

        //  Read the directory block

        auto read_block_result = block_io_adapter.ReadCluster(address.Cluster(), block_buffer);

        if (read_block_result != BlockIOResultCodes::SUCCESS)
        {
            return FilesystemResultCodes::FAT32_DEVICE_READ_ERROR;
        }

        FAT32DirectoryClusterEntry &entry = ((FAT32DirectoryClusterEntry *)block_buffer)[address.Index()];

        entry.SetSize(new_size);
        entry.SetArchive();

        auto write_block_result = block_io_adapter.WriteCluster(address.Cluster(), block_buffer);

        if (write_block_result != BlockIOResultCodes::SUCCESS)
        {
            return FilesystemResultCodes::FAT32_DEVICE_WRITE_ERROR;
        }

        return FilesystemResultCodes::SUCCESS;
    }

    FilesystemResultCodes FAT32Directory::DeleteFile(const minstd::string &filename)
    {
        using Result = FilesystemResultCodes;

        LogEntryAndExit("Entering with filename: %s\n", filename.c_str());

        //  Get the filesystem entity

        auto locked_filesystem = FAT32Filesystem::Lock(FilesystemUUID());

        if (!locked_filesystem)
        {
            return FilesystemResultCodes::FILESYSTEM_DOES_NOT_EXIST;
        }

        FAT32Filesystem &filesystem = *locked_filesystem;
        ReturnOnCallFailure(ValidateHandle(filesystem));

        //  Get the block io adapter from the filesystem

        FAT32BlockIOAdapter &block_io_adapter = filesystem.BlockIOAdapter();

        //  Return an error if the file does not exist

        auto file_entry = GetEntry(block_io_adapter, filename, FilesystemDirectoryEntryType::FILE);

        if (!file_entry.Successful())
        {
            return FilesystemResultCodes::FILE_NOT_FOUND;
        }

        //  Insure the file is not open

        if (GetFileMap().IsFileOpen(FAT32OpenFileIdentity(FilesystemUUID(), GetOpaqueData(*file_entry).directory_entry_address_)))
        {
            return FilesystemResultCodes::FILE_ALREADY_OPENED_EXCLUSIVELY;
        }

        //  Remove any entry from the cache first

        filesystem.DirectoryCache().RemoveEntry(GetOpaqueData(*file_entry).FirstCluster());

        //  Get the directory cluster

        FAT32DirectoryCluster directory_cluster = FAT32DirectoryCluster(filesystem.Id(),
                                                                        block_io_adapter,
                                                                        FirstCluster());

        //  Insure the file still exists, if not return a file not found error

        auto cluster_entry = directory_cluster.GetClusterEntry(GetOpaqueData(*file_entry).directory_entry_address_);

        ReturnOnFailure(cluster_entry);

        if (cluster_entry->IsUnused() || !cluster_entry->IsStandardEntry())
        {
            return FilesystemResultCodes::FILE_NOT_FOUND; //  We should never end up here....
        }

        //  Remove the file cluster entry

        ReturnOnCallFailure(directory_cluster.RemoveEntry(GetOpaqueData(*file_entry).directory_entry_address_));

        //  Release the clusters for the file.  A file that was never written has no chain.

        const FAT32ClusterIndex file_first_cluster = cluster_entry->FirstCluster(block_io_adapter.RootDirectoryCluster());

        if ((uint32_t)file_first_cluster != 0)
        {
            ReturnOnCallFailure(block_io_adapter.ReleaseChain(file_first_cluster));
        }

        //  Finished with Success

        return FilesystemResultCodes::SUCCESS;
    }

    FilesystemResultCodes FAT32Directory::RenameEntry(const minstd::string &name, const minstd::string &new_name, FilesystemDirectoryEntryType entry_type)
    {
        using Result = FilesystemResultCodes;

        LogEntryAndExit("Entering with directory name: %s\n", name.c_str());

        //  Get the filesystem entity

        auto locked_filesystem = FAT32Filesystem::Lock(FilesystemUUID());

        if (!locked_filesystem)
        {
            return FilesystemResultCodes::FILESYSTEM_DOES_NOT_EXIST;
        }

        FAT32Filesystem &filesystem = *locked_filesystem;
        ReturnOnCallFailure(ValidateHandle(filesystem));

        //  Get the block io adapter from the filesystem

        FAT32BlockIOAdapter &block_io_adapter = filesystem.BlockIOAdapter();

        //  Get the directory cluster

        FAT32DirectoryCluster directory_cluster = FAT32DirectoryCluster(filesystem.Id(),
                                                                        block_io_adapter,
                                                                        FirstCluster());

        //  Return an error if the file does not exist

        auto directory_entry = GetEntry(block_io_adapter, name, entry_type);

        ReturnOnFailure(directory_entry);

        //  Can't rename a file that is currently open exclusively by someone else

        if ((entry_type == FilesystemDirectoryEntryType::FILE) &&
            GetFileMap().IsFileOpen(FAT32OpenFileIdentity(FilesystemUUID(), GetOpaqueData(*directory_entry).directory_entry_address_)))
        {
            return FilesystemResultCodes::FILE_ALREADY_OPENED_EXCLUSIVELY;
        }

        //  Create the new directory entry

        auto new_directory_entry = directory_cluster.CreateEntry(new_name,
                                                                 GetOpaqueData(*directory_entry).FirstCluster(),
                                                                 GetOpaqueData(*directory_entry).directory_entry_);

        ReturnOnFailure(new_directory_entry);

        //  Remove any cache entries and the directory entry

        if (entry_type == FilesystemDirectoryEntryType::DIRECTORY)
        {
            filesystem.DirectoryCache().Clear();
            filesystem.InvalidateDirectoryHandles();
        }
        else
        {
            filesystem.DirectoryCache().RemoveEntry(GetOpaqueData(*directory_entry).FirstCluster());
        }

        //  Both entries now reference the same cluster chain.  If the old one cannot be removed,
        //      remove the new one rather than leave the chain cross-linked.  The rollback is best
        //      effort: the caller needs the original failure, not the rollback's.

        const FilesystemResultCodes remove_result = directory_cluster.RemoveEntry(GetOpaqueData(*directory_entry).directory_entry_address_);

        if (remove_result != FilesystemResultCodes::SUCCESS)
        {
            directory_cluster.RemoveEntry(GetOpaqueData(*new_directory_entry).directory_entry_address_);

            return remove_result;
        }

        //  Finished with Success

        return FilesystemResultCodes::SUCCESS;
    }
} // namespace filesystems::fat32
