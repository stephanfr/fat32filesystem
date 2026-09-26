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
}
