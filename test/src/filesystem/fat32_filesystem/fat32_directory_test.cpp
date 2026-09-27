// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "../../cpputest_support.h"

#include "../../utility/mounted_test_filesystem.h"

#include "filesystem/fat32_directory.h"
#include "filesystem/fat32_filesystem.h"
#include "filesystem/file_map.h"

namespace
{
    using namespace filesystems;
    using namespace filesystems::fat32;

    ut_utility::MountedTestFilesystem test_fs;

    //  Counts long filename entries still marked in use inside one directory cluster.
    //      Attribute 0x0F marks an LFN entry; byte 0 of 0xE5 marks it deleted and 0x00 marks
    //      the end of the directory.

    uint32_t CountLiveLFNEntries(FAT32ClusterIndex cluster)
    {
        minstd::heap_buffer<uint8_t> buffer(__os_dynamic_heap_resource, test_fs.Adapter().BytesPerCluster());

        CHECK(test_fs.ReadRawCluster(cluster, buffer.data()));

        const uint32_t entries_per_cluster = test_fs.Adapter().BytesPerCluster() / 32;

        uint32_t count = 0;

        for (uint32_t i = 0; i < entries_per_cluster; i++)
        {
            const uint8_t *entry = buffer.data() + (i * 32);

            if ((entry[11] == 0x0F) && (entry[0] != 0xE5) && (entry[0] != 0x00))
            {
                count++;
            }
        }

        return count;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (FAT32DirectoryTest)
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

    TEST(FAT32DirectoryTest, RemoveDirectoryRejectsNonEmptyDirectory)
    {
        auto root = test_fs.RootDirectory();

        auto subdir = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir.Successful());

        //  Record a child's first cluster so we can prove it was not orphaned.

        auto child = (*subdir)->GetDirectory(minstd::fixed_string<MAX_FILENAME_LENGTH>("this is a long subdirectory name"));

        CHECK(child.Successful());

        FAT32ClusterIndex child_cluster = static_cast<FAT32Directory *>(child.Value().get())->FirstCluster();

        auto before = test_fs.Adapter().NextClusterInChain(child_cluster);

        CHECK(before.Successful());

        //  The removal must be refused outright.

        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_NOT_EMPTY, (*subdir)->RemoveDirectory());

        //  And the child's chain must be untouched.  Without the guard the parent's chain was
        //      released and every descendant's clusters were left allocated but unreachable.

        auto after = test_fs.Adapter().NextClusterInChain(child_cluster);

        CHECK(after.Successful());
        CHECK_EQUAL((uint32_t)before.Value(), (uint32_t)after.Value());
    }

    TEST(FAT32DirectoryTest, StaleDirectoryHandleCannotRemoveTheDirectoryThatReusedItsSlot)
    {
        auto root = test_fs.RootDirectory();

        const FAT32ClusterIndex root_cluster = test_fs.Adapter().RootDirectoryCluster();

        auto second_cluster = test_fs.Adapter().NextClusterInChain(root_cluster);      //  cluster 12

        CHECK(second_cluster.Successful());

        //  Root cluster 2 is full and cluster 12 has idx 0-1 in use, so an 8.3 directory
        //      (which needs a 2-slot hole) lands at cluster 12 idx 2.

        CHECK(root->CreateDirectory(minstd::fixed_string<>("OLDDIR")).Successful());

        auto first = root->GetDirectory(minstd::fixed_string<>("OLDDIR"));
        auto stale = root->GetDirectory(minstd::fixed_string<>("OLDDIR"));

        CHECK(first.Successful());
        CHECK(stale.Successful());

        const FAT32ClusterIndex olddir_cluster = static_cast<FAT32Directory *>(stale.Value().get())->FirstCluster();

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*first)->RemoveDirectory());

        //  idx 2 is now 0xE5 with free space behind it, so the next 8.3 directory reuses
        //      idx 2 - the address the stale handle still holds.  Its first cluster differs,
        //      because FindNextEmptyCluster searches from its high-water mark.

        auto newdir = root->CreateDirectory(minstd::fixed_string<>("NEWDIR"));

        CHECK(newdir.Successful());

        const FAT32ClusterIndex newdir_cluster = static_cast<FAT32Directory *>(newdir.Value().get())->FirstCluster();

        //  Premise: the slot really was reused, by a directory with a different first cluster.

        minstd::heap_buffer<uint8_t> raw_cluster(__os_dynamic_heap_resource, test_fs.Adapter().BytesPerCluster());

        CHECK(test_fs.ReadRawCluster(*second_cluster, raw_cluster.data()));
        CHECK(memcmp(raw_cluster.data() + (2 * 32), "NEWDIR     ", 11) == 0);
        CHECK((uint32_t)newdir_cluster != (uint32_t)olddir_cluster);

        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_NOT_FOUND, (*stale)->RemoveDirectory());

        CHECK(root->GetDirectory(minstd::fixed_string<>("NEWDIR")).Successful());
    }
}
