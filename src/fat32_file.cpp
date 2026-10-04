// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <stdint.h>

#include "filesystem/file_map.h"

#include "filesystem/fat32_file.h"
#include "filesystem/fat32_filesystem.h"

namespace filesystems::fat32
{
    FilesystemResultCodes FAT32File::SeekEnd()
    {
        return Seek(directory_entry_.Size());
    }

    FilesystemResultCodes FAT32File::Seek(uint32_t position)
    {
        using Result = FilesystemResultCodes;

        LogEntryAndExit("Entering\n");

        //  Get the filesystem entity

        auto locked_filesystem = FAT32Filesystem::Lock(filesystem_uuid_);

        if (!locked_filesystem)
        {
            return FilesystemResultCodes::FILESYSTEM_DOES_NOT_EXIST;
        }

        FAT32Filesystem &filesystem = *locked_filesystem;

        //  Get the block io adapter from the filesystem

        FAT32BlockIOAdapter &block_io_adapter = filesystem.BlockIOAdapter();

        //  If the current cluster is zero, then we have an empty file and are already at the end

        if (current_cluster_ == 0)
        {
            return Result::SUCCESS;
        }

        //  If position is zero, then we are at the start of the file now

        if (position == 0)
        {
            current_cluster_ = first_cluster_;
            byte_offset_into_cluster_ = 0;
            byte_offset_into_file_ = 0;

            return Result::SUCCESS;
        }

        //  Set position to the smaller of the position or the file size

        position = minstd::min(position, directory_entry_.Size());

        //  If the target position is before the current position, reset to the file start.
        //  This ensures absolute seeks move backwards as well as forwards.

        if (position < byte_offset_into_file_)
        {
            current_cluster_ = first_cluster_;
            byte_offset_into_cluster_ = 0;
            byte_offset_into_file_ = 0;
        }

        //  Read from the current cluster and offset and append to the buffer until the buffer is full.

        uint32_t bytes_in_block = block_io_adapter.BytesPerCluster();

        while (true)
        {
            //  We will either seek to position or the end of the current block

            uint32_t bytes_to_read = minstd::min(bytes_in_block - byte_offset_into_cluster_, position - byte_offset_into_file_);

            byte_offset_into_file_ += bytes_to_read;
            byte_offset_into_cluster_ += bytes_to_read;

            //  Break if we reached the end of the file

            if (byte_offset_into_file_ >= position)
            {
                break;
            }

            //  Move to the next block

            if (byte_offset_into_cluster_ >= bytes_in_block)
            {
                auto next_file_cluster = block_io_adapter.NextClusterInChain(current_cluster_);

                ReturnOnFailure(next_file_cluster);

                if (*next_file_cluster >= FAT32EntryEOFThreshold)
                {
                    break;
                }

                current_cluster_ = *next_file_cluster;
                byte_offset_into_cluster_ = 0;
            }
        }

        return FilesystemResultCodes::SUCCESS;
    }

    FilesystemResultCodes FAT32File::Read(minstd::buffer<uint8_t> &buffer)
    {
        using Result = FilesystemResultCodes;

        LogEntryAndExit("Entering\n");

        //  Get the filesystem entity

        auto locked_filesystem = FAT32Filesystem::Lock(filesystem_uuid_);

        if (!locked_filesystem)
        {
            return FilesystemResultCodes::FILESYSTEM_DOES_NOT_EXIST;
        }

        FAT32Filesystem &filesystem = *locked_filesystem;

        //  Get the block io adapter from the filesystem

        FAT32BlockIOAdapter &block_io_adapter = filesystem.BlockIOAdapter();

        //  Create a read buffer

        minstd::heap_buffer<uint8_t> cluster_buffer(__os_dynamic_heap_resource, block_io_adapter.BytesPerCluster());
        uint8_t *block_buffer = cluster_buffer.data();
        uint32_t bytes_in_block = block_io_adapter.BytesPerCluster();

        //  If the current cluster is zero, then we have an empty file and there is nothing to read

        if (current_cluster_ == 0)
        {
            return Result::SUCCESS;
        }

        //  Read from the current cluster and offset and append to the buffer until the buffer is full.

        while (buffer.space_remaining() > 0)
        {
            auto read_block_result = block_io_adapter.ReadCluster(current_cluster_, block_buffer);

            if (read_block_result != BlockIOResultCodes::SUCCESS)
            {
                return FilesystemResultCodes::FAT32_DEVICE_READ_ERROR;
            }

            //  Read the minimum of the number of bytes not yet read from the cluster or the number of bytes remaining in the file.

            uint32_t bytes_to_read = minstd::min(bytes_in_block - byte_offset_into_cluster_, directory_entry_.Size() - byte_offset_into_file_);

            //  Append to the buffer, though the number of bytes appended may be less than the bytes to read if we run out of space in the buffer

            uint32_t bytes_appended = buffer.append(block_buffer + byte_offset_into_cluster_, bytes_to_read);

            byte_offset_into_file_ += bytes_appended;
            byte_offset_into_cluster_ += bytes_appended;

            //  Break if we have read the whole file

            if (byte_offset_into_file_ >= directory_entry_.Size())
            {
                break;
            }

            //  If we have read all the bytes in the block, then move to the next block

            if (byte_offset_into_cluster_ >= bytes_in_block)
            {
                auto next_file_cluster = block_io_adapter.NextClusterInChain(current_cluster_);

                ReturnOnFailure(next_file_cluster);

                //  We are done if we have hit the last block.

                if (*next_file_cluster >= FAT32EntryEOFThreshold)
                {
                    break;
                }

                current_cluster_ = *next_file_cluster;
                byte_offset_into_cluster_ = 0;
            }
        }

        return FilesystemResultCodes::SUCCESS;
    }

    FilesystemResultCodes FAT32File::Write(const minstd::buffer<uint8_t> &buffer)
    {
        using Result = FilesystemResultCodes;

        //  Insure the file is opened for write

        if (!HasFileMode(mode_, FileModes::WRITE) && !HasFileMode(mode_, FileModes::APPEND))
        {
            return FilesystemResultCodes::FILE_NOT_OPENED_FOR_WRITE;
        }

        //  Nothing to write: in particular, do not give an empty file a cluster - a 0-byte file
        //      must have first cluster 0.

        if (buffer.size() == 0)
        {
            return FilesystemResultCodes::SUCCESS;
        }

        //  Get the filesystem entity

        auto locked_filesystem = FAT32Filesystem::Lock(filesystem_uuid_);

        if (!locked_filesystem)
        {
            return FilesystemResultCodes::FILESYSTEM_DOES_NOT_EXIST;
        }

        FAT32Filesystem &filesystem = *locked_filesystem;

        //  An append-mode handle always writes at the end of the file.

        if (HasFileMode(mode_, FileModes::APPEND))
        {
            ReturnOnCallFailure(SeekEnd());
        }

        //  Get the block io adapter from the filesystem

        FAT32BlockIOAdapter &block_io_adapter = filesystem.BlockIOAdapter();

        //  If the current cluster is zero, then we have an empty file so we have to allocate a cluster now

        if (current_cluster_ == 0)
        {
            auto new_cluster_index = block_io_adapter.FindNextEmptyCluster();

            ReturnOnFailure(new_cluster_index);

            if (block_io_adapter.UpdateFATTableEntry(*new_cluster_index, FAT32EntryAllocatedAndEndOfFile) != FilesystemResultCodes::SUCCESS)
            {
                return FilesystemResultCodes::FAT32_UNABLE_TO_WRITE_FAT_TABLE_SECTOR;
            }

            //  Update the directory entry with the initial cluster

            ReturnOnCallFailure(FAT32Directory::SetDirectoryEntryFirstCluster(block_io_adapter, directory_entry_address_, *new_cluster_index));

            //  Move to the new cluster

            first_cluster_ = *new_cluster_index;
            current_cluster_ = *new_cluster_index;
        }

        //  Allocate a buffer for the cluster

        minstd::heap_buffer<uint8_t> cluster_buffer(__os_dynamic_heap_resource, block_io_adapter.BytesPerCluster());
        uint8_t *block_buffer = cluster_buffer.data();

        //  Start tracking the offset into the write buffer

        uint32_t offset_into_buffer = 0;

        while (offset_into_buffer < buffer.size())
        {
            uint32_t bytes_left_in_cluster = block_io_adapter.BytesPerCluster() - byte_offset_into_cluster_;
            uint32_t bytes_to_copy = minstd::min(bytes_left_in_cluster, (uint32_t)buffer.size() - offset_into_buffer);

            //  The whole cluster is written back below, so unless every byte of it is being replaced it must be read first.

            const bool overwrites_whole_cluster = (byte_offset_into_cluster_ == 0) &&
                                                  (bytes_to_copy == block_io_adapter.BytesPerCluster());

            if (!overwrites_whole_cluster)
            {
                if (block_io_adapter.ReadCluster(current_cluster_, block_buffer) != BlockIOResultCodes::SUCCESS)
                {
                    return FilesystemResultCodes::FAT32_DEVICE_READ_ERROR;
                }
            }

            memcpy(block_buffer + byte_offset_into_cluster_, (char *)buffer.data() + offset_into_buffer, bytes_to_copy);

            BlockIOResultCodes write_block_result = block_io_adapter.WriteCluster(current_cluster_, block_buffer);

            if (write_block_result != BlockIOResultCodes::SUCCESS)
            {
                LogDebug1("Writing cluster failed with code: %d\n", write_block_result);
                return FilesystemResultCodes::FAT32_DEVICE_WRITE_ERROR;
            }

            //  Move forward in the cluster

            byte_offset_into_file_ += bytes_to_copy;
            byte_offset_into_cluster_ += bytes_to_copy;

            //  Move forward in the append buffer.  Break now if the buffer has been fully written.

            offset_into_buffer += bytes_to_copy;

            if (offset_into_buffer >= buffer.size())
            {
                break;
            }

            //  There is still data to be written to the device, which means we need to move forward to the next
            //      cluster in the file -or- get a new cluster if we are at the end of the file.

            if (byte_offset_into_file_ < directory_entry_.Size())
            {
                auto next_cluster = block_io_adapter.NextClusterInChain(current_cluster_);

                ReturnOnFailure(next_cluster);

                current_cluster_ = *next_cluster;
                byte_offset_into_cluster_ = 0;

                continue;
            }

            //  OK, we have filled the existing file storage so we need a new cluster to continue.
            //
            //  Get the next empty cluster and claim it as the new end of the file, then link the previous final
            //      cluster to it.  Claiming first means the chain never points at a free cluster.

            auto next_empty_cluster = block_io_adapter.FindNextEmptyCluster(current_cluster_ + 1);

            ReturnOnFailure(next_empty_cluster);

            ReturnOnCallFailure(block_io_adapter.UpdateFATTableEntry(*next_empty_cluster, FAT32EntryAllocatedAndEndOfFile));

            const FilesystemResultCodes link_result = block_io_adapter.UpdateFATTableEntry(current_cluster_, *next_empty_cluster);

            if (link_result != FilesystemResultCodes::SUCCESS)
            {
                //  Release the unlinked cluster.  The rollback is best effort: the caller needs the
                //      original failure, not the rollback's.

                block_io_adapter.UpdateFATTableEntry(*next_empty_cluster, FAT32EntryFree);

                return link_result;
            }
            
            //  Move to the next cluster and reset the offset into the cluster to zero.

            current_cluster_ = *next_empty_cluster;
            byte_offset_into_cluster_ = 0;
        }

        //  Finally, update the directory entry, in memory and on disk: the size if the file grew, and
        //      ATTR_ARCHIVE the first time this handle modifies the file - even if the size is unchanged.

        const bool file_grew = byte_offset_into_file_ > directory_entry_.Size();

        if (file_grew || !marked_archive_)
        {
            if (file_grew)
            {
                directory_entry_.UpdateSize(byte_offset_into_file_);
            }

            auto update_directory_entry_result = FAT32Directory::UpdateDirectoryEntrySize(block_io_adapter, directory_entry_address_, directory_entry_.Size());

            if (update_directory_entry_result != FilesystemResultCodes::SUCCESS)
            {
                LogDebug1("Failed to update directory entry after write\n");

                return update_directory_entry_result;
            }

            marked_archive_ = true;
        }

        //  Finished with success

        return FilesystemResultCodes::SUCCESS;
    }

    FilesystemResultCodes FAT32File::Append(const minstd::buffer<uint8_t> &buffer)
    {
        LogEntryAndExit("Entering\n");

        //  Reject before seeking - a read-only handle must not have its position moved by a
        //      call it was never allowed to make.

        if (!HasFileMode(mode_, FileModes::WRITE) && !HasFileMode(mode_, FileModes::APPEND))
        {
            return FilesystemResultCodes::FILE_NOT_OPENED_FOR_WRITE;
        }

        //  Get a lock on the filesystem to ensure thread safety - Write will just get the same lock recursively.

        auto locked_filesystem = FAT32Filesystem::Lock(filesystem_uuid_);

        if (!locked_filesystem)
        {
            return FilesystemResultCodes::FILESYSTEM_DOES_NOT_EXIST;
        }

        //  Move to the end of the file

        FilesystemResultCodes seek_result = SeekEnd();
        if (seek_result != FilesystemResultCodes::SUCCESS)
        {
            return seek_result;
        }

        return Write(buffer);
    }

    FilesystemResultCodes FAT32File::Close()
    {
        LogDebug1("Closing file: %s\n", path_.c_str());

        auto locked_filesystem = FAT32Filesystem::Lock(filesystem_uuid_);

        const FilesystemResultCodes result = GetFileMap().RemoveFile(*this);

        return result;
    }
} // namespace filesystems::fat32
