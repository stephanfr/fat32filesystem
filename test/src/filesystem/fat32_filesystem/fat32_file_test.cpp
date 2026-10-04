// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "../../cpputest_support.h"

#include "../../utility/mounted_test_filesystem.h"

#include "filesystem/fat32_directory.h"
#include "filesystem/fat32_file.h"
#include "filesystem/fat32_filesystem.h"
#include "filesystem/file_map.h"

namespace
{
    using namespace filesystems;
    using namespace filesystems::fat32;

    ut_utility::MountedTestFilesystem test_fs;

    void FillBuffer(minstd::buffer<uint8_t> & buffer, uint8_t value, uint32_t count)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            uint8_t byte = value;

            buffer.append(&byte, 1);
        }
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (FAT32FileTest)
    {
        size_t heap_bytes_at_start_ = 0;

        void setup()
        {
            heap_bytes_at_start_ = __os_dynamic_heap_core.bytes_in_use();
            CHECK(test_fs.Mount());
        }

        void teardown()
        {
            GetFileMap().Clear();
            test_fs.Unmount();
            CHECK_EQUAL(heap_bytes_at_start_, __os_dynamic_heap_core.bytes_in_use());
        }
    };
#pragma GCC diagnostic pop

    TEST(FAT32FileTest, WritePreservesTrailingBytesOfCluster)
    {
        auto root = test_fs.RootDirectory();

        const uint32_t bytes_per_cluster = test_fs.Adapter().BytesPerCluster();

        //  Create a file and fill exactly one cluster with 0xAA.

        auto file = root->OpenFile(minstd::fixed_string<>("scratch.dat"),
                                   static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));

        CHECK(file.Successful());

        minstd::heap_buffer<uint8_t> full_cluster(__os_dynamic_heap_resource, bytes_per_cluster);

        FillBuffer(full_cluster, 0xAA, bytes_per_cluster);

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Write(full_cluster));
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());

        //  Re-open, seek to the start, overwrite only the first 100 bytes with 0xBB.

        auto rewrite = root->OpenFile(minstd::fixed_string<>("scratch.dat"), FileModes::WRITE);

        CHECK(rewrite.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*rewrite)->Seek(0));

        minstd::heap_buffer<uint8_t> patch(__os_dynamic_heap_resource, 100);

        FillBuffer(patch, 0xBB, 100);

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*rewrite)->Write(patch));
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*rewrite)->Close());

        //  Every byte past the patch must still be 0xAA.  Pre-fix they are whatever happened
        //      to be on the stack, because the cluster was never read back in.

        auto verify = root->OpenFile(minstd::fixed_string<>("scratch.dat"), FileModes::READ);

        CHECK(verify.Successful());

        minstd::heap_buffer<uint8_t> read_back(__os_dynamic_heap_resource, bytes_per_cluster);

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*verify)->Read(read_back));
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*verify)->Close());

        for (uint32_t i = 0; i < 100; i++)
        {
            CHECK_EQUAL(0xBB, read_back.data()[i]);
        }

        for (uint32_t i = 100; i < bytes_per_cluster; i++)
        {
            CHECK_EQUAL(0xAA, read_back.data()[i]);
        }

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, root->DeleteFile(minstd::fixed_string<>("scratch.dat")));
    }

    TEST(FAT32FileTest, ReadOnlyHandlesCannotWrite)
    {
        auto root = test_fs.RootDirectory();

        auto subdir = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir.Successful());

        //  FileMap allows both of these opens precisely because readers are assumed unable
        //      to mutate the file.  Enforce that assumption.

        minstd::fixed_string<MAX_FILENAME_LENGTH> name("Lorem ipsum dolor sit amet.text");

        auto first = (*subdir)->OpenFile(name, FileModes::READ);
        auto second = (*subdir)->OpenFile(name, FileModes::READ);

        CHECK(first.Successful());
        CHECK(second.Successful());

        minstd::heap_buffer<uint8_t> payload(__os_dynamic_heap_resource, 16);

        FillBuffer(payload, 0xEE, 16);

        CHECK_EQUAL(FilesystemResultCodes::FILE_NOT_OPENED_FOR_WRITE, (*first)->Write(payload));
        CHECK_EQUAL(FilesystemResultCodes::FILE_NOT_OPENED_FOR_WRITE, (*first)->Append(payload));
        CHECK_EQUAL(FilesystemResultCodes::FILE_NOT_OPENED_FOR_WRITE, (*second)->Write(payload));

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*first)->Close());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*second)->Close());
    }

    TEST(FAT32FileTest, CloseRemovesFileFromMap)
    {
        auto root = test_fs.RootDirectory();

        auto subdir = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir.Successful());

        const OpenFileIdentity lorem = FAT32OpenFileIdentity(test_fs.FilesystemUUID(), FAT32DirectoryEntryAddress(FAT32ClusterIndex(3), 14));

        auto file = (*subdir)->OpenFile(minstd::fixed_string<MAX_FILENAME_LENGTH>("Lorem ipsum dolor sit amet.text"), FileModes::READ);

        CHECK(file.Successful());
        CHECK(GetFileMap().IsFileOpen(lorem));

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());

        CHECK_FALSE(GetFileMap().IsFileOpen(lorem));

        //  A second Close() on the same handle is a use-after-free until RemoveFile hands back
        //      the owning unique_ptr.  Do not add that call here until it does.
    }

    TEST(FAT32FileTest, RejectedAppendDoesNotMoveTheReadPosition)
    {
        auto root = test_fs.RootDirectory();

        auto subdir = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir.Successful());

        auto file = (*subdir)->OpenFile(minstd::fixed_string<MAX_FILENAME_LENGTH>("Lorem ipsum dolor sit amet.text"), FileModes::READ);

        CHECK(file.Successful());

        minstd::heap_buffer<uint8_t> payload(__os_dynamic_heap_resource, 16);

        FillBuffer(payload, 0xEE, 16);

        CHECK_EQUAL(FilesystemResultCodes::FILE_NOT_OPENED_FOR_WRITE, (*file)->Append(payload));

        //  The handle was never allowed to append, so it must still be at the start of the 992-byte file.

        minstd::heap_buffer<uint8_t> read_back(__os_dynamic_heap_resource, 1024);

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Read(read_back));
        CHECK_EQUAL(992, read_back.size());

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());
    }

    TEST(FAT32FileTest, EmptyWriteDoesNotAllocate)
    {
        auto root = test_fs.RootDirectory();

        const auto before = test_fs.Adapter().FindNextEmptyCluster(FAT32ClusterIndex(40));

        CHECK(before.Successful());

        auto file = root->OpenFile(minstd::fixed_string<>("empty.dat"), static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));

        CHECK(file.Successful());

        minstd::heap_buffer<uint8_t> nothing(__os_dynamic_heap_resource, 16);       //  capacity 16, size 0

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Write(nothing));
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());

        //  The cluster that was free before is still free.

        CHECK_SUCCESSFUL_AND_EQUAL((uint32_t)*before, test_fs.Adapter().FindNextEmptyCluster(FAT32ClusterIndex(40)));
    }

    TEST(FAT32FileTest, WriteOnAnAppendHandleAppends)
    {
        auto root = test_fs.RootDirectory();
        auto subdir = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir.Successful());

        minstd::fixed_string<MAX_FILENAME_LENGTH> name("Lorem ipsum dolor sit amet.text");

        auto appender = (*subdir)->OpenFile(name, FileModes::APPEND);

        CHECK(appender.Successful());

        minstd::heap_buffer<uint8_t> payload(__os_dynamic_heap_resource, 16);

        FillBuffer(payload, 0xEE, 16);

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*appender)->Write(payload));
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*appender)->Close());

        auto reader = (*subdir)->OpenFile(name, FileModes::READ);

        CHECK(reader.Successful());

        minstd::heap_buffer<uint8_t> contents(__os_dynamic_heap_resource, 2048);

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*reader)->Read(contents));
        CHECK_EQUAL(992 + 16, contents.size());
        CHECK_EQUAL('L', contents.data()[0]);
        CHECK_EQUAL(0xEE, contents.data()[992]);
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*reader)->Close());
    }

    TEST(FAT32FileTest, FailedChainGrowthNeverLinksAFreeCluster)
    {
        FAT32BlockIOAdapter &adapter = test_fs.Adapter();

        const uint32_t bytes_per_cluster = adapter.BytesPerCluster();

        //  An empty file's first cluster is the one the allocator hands out next.

        auto first_cluster = adapter.FindNextEmptyCluster();

        CHECK(first_cluster.Successful());

        auto root = test_fs.RootDirectory();

        auto file = root->OpenFile(minstd::fixed_string<>("grow.dat"), static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));

        CHECK(file.Successful());

        //  Fill exactly one cluster.  This is also the first FAT change since mount, so FSInfo is
        //      invalidated here and adds no write below.

        minstd::heap_buffer<uint8_t> full_cluster(__os_dynamic_heap_resource, bytes_per_cluster);

        FillBuffer(full_cluster, 0xAA, bytes_per_cluster);

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Write(full_cluster));
        CHECK(*adapter.NextClusterInChain(*first_cluster) >= FAT32EntryEOFThreshold);

        //  Growing by one byte writes: #1 the current cluster, then two FAT updates of two writes
        //      each (primary FAT, then mirror).  Fail write #4 - the second update's primary write.

        test_fs.Device().SimulateWriteError(3);

        minstd::heap_buffer<uint8_t> one_byte(__os_dynamic_heap_resource, 1);

        FillBuffer(one_byte, 0xBB, 1);

        CHECK_EQUAL(FilesystemResultCodes::FAT32_UNABLE_TO_WRITE_FAT_TABLE_SECTOR, (*file)->Write(one_byte));

        //  Whatever the first cluster now points to must be allocated - never free.

        auto next = adapter.NextClusterInChain(*first_cluster);

        CHECK(next.Successful());

        if (*next < FAT32EntryEOFThreshold)
        {
            auto after = adapter.NextClusterInChain(*next);

            CHECK(after.Successful());
            CHECK(*after != FAT32EntryFree);
        }

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());
    }

    TEST(FAT32FileTest, WritingAFileMarksItArchiveAndReadingDoesNot)
    {
        //  Clear ARCHIVE on Lorem's short entry (SUBDIR1 cluster 3 idx 14).

        const uint32_t attribute_offset = (14 * 32) + 11;

        minstd::heap_buffer<uint8_t> raw(__os_dynamic_heap_resource, test_fs.Adapter().BytesPerCluster());

        CHECK(test_fs.ReadRawCluster(FAT32ClusterIndex(3), raw.data()));
        raw.data()[attribute_offset] = 0x00;
        CHECK(test_fs.Adapter().WriteCluster(FAT32ClusterIndex(3), raw.data()) == BlockIOResultCodes::SUCCESS);

        auto root = test_fs.RootDirectory();
        auto subdir = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir.Successful());

        minstd::fixed_string<MAX_FILENAME_LENGTH> name("Lorem ipsum dolor sit amet.text");

        //  Reading is not a modification.

        auto reader = (*subdir)->OpenFile(name, FileModes::READ);

        CHECK(reader.Successful());

        minstd::heap_buffer<uint8_t> contents(__os_dynamic_heap_resource, 2048);

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*reader)->Read(contents));
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*reader)->Close());

        CHECK(test_fs.ReadRawCluster(FAT32ClusterIndex(3), raw.data()));
        CHECK_EQUAL(0x00, raw.data()[attribute_offset]);

        //  Overwriting in place is - even though the size does not change.

        auto writer = (*subdir)->OpenFile(name, FileModes::WRITE);

        CHECK(writer.Successful());

        minstd::heap_buffer<uint8_t> payload(__os_dynamic_heap_resource, 16);

        FillBuffer(payload, 0xEE, 16);

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*writer)->Write(payload));
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*writer)->Close());

        CHECK(test_fs.ReadRawCluster(FAT32ClusterIndex(3), raw.data()));
        CHECK_EQUAL(0x20, raw.data()[attribute_offset]);
    }
}
