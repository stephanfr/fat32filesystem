// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "../../cpputest_support.h"

#include <__memory_resource/monotonic_buffer_resource.h>
#include <__memory_resource/polymorphic_allocator.h>

#include "../../utility/in_memory_blockio_device.h"

#include "filesystem/fat32_directory_cluster.h"
#include "filesystem/fat32_filesystem.h"
#include "filesystem/fat32_partition.h"
#include "filesystem/master_boot_record.h"

namespace
{
    using namespace filesystems;
    using namespace filesystems::fat32;

    minstd::unique_ptr<ut_utility::InMemoryFileBlockIODevice> test_device;

    alignas(MassStoragePartition) uint8_t partition_buffer[
        sizeof(MassStoragePartition) * MAX_PARTITIONS_ON_MASS_STORAGE_DEVICE +
        alignof(MassStoragePartition) * MAX_PARTITIONS_ON_MASS_STORAGE_DEVICE];
    minstd::pmr::monotonic_buffer_resource partition_resource(partition_buffer, sizeof(partition_buffer), nullptr);
    minstd::pmr::polymorphic_allocator<MassStoragePartition> partition_allocator(&partition_resource);

    MassStoragePartitions partitions(partition_allocator);

    //  Walks a directory with the iterator.  Returns the entry count, or -1 if iteration
    //      reported a failure or ran away.

    int32_t CountEntries(FAT32DirectoryCluster & directory)
    {
        auto itr = directory.directory_entry_iterator_begin();

        int32_t count = 0;

        while (!itr.end())
        {
            if (itr.AsClusterEntry().Failed())
            {
                return -1;
            }

            count++;

            if (itr++ != FilesystemResultCodes::SUCCESS)
            {
                return -1;
            }

            if (count > 100000)
            {
                return -1;
            }
        }

        return count;
    }

    //  Re-opens the image from disk, truncates the root directory's cluster chain with the
    //      given end-of-chain marker, and returns how many entries the iterator yields.
    //      Returns -1 if iteration reported a failure.  Each call starts from a pristine image.

    int32_t CountRootEntriesWithChainTerminator(uint32_t marker)
    {
        test_device = make_dynamic_unique<ut_utility::InMemoryFileBlockIODevice>("IN_MEMORY_TEST_DEVICE");

        if (!test_device->Open("./test/data/test_fat32.img"))
        {
            return -1;
        }

        partitions.clear();

        if (GetPartitions(*test_device, partitions) != FilesystemResultCodes::SUCCESS)
        {
            return -1;
        }

        auto test_fat32 = FAT32Filesystem::Mount(false, "test_fat32", "TESTFAT32", false, *test_device, partitions[0]);

        if (!test_fat32.Successful())
        {
            return -1;
        }

        FAT32ClusterIndex root_cluster = test_fat32->BlockIOAdapter().RootDirectoryCluster();

        if (test_fat32->BlockIOAdapter().UpdateFATTableEntry(root_cluster, FAT32ClusterIndex(marker)) != FilesystemResultCodes::SUCCESS)
        {
            return -1;
        }

        FAT32DirectoryCluster directory(test_fat32->Id(), test_fat32->BlockIOAdapter(), root_cluster);

        return CountEntries(directory);
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (FAT32DirectoryClusterTest)
    {
        size_t heap_bytes_at_start_ = 0;

        void setup()
        {
            heap_bytes_at_start_ = __os_dynamic_heap_core.bytes_in_use();

            test_device = make_dynamic_unique<ut_utility::InMemoryFileBlockIODevice>("IN_MEMORY_TEST_DEVICE");

            CHECK(test_device->Open("./test/data/test_fat32.img"));
            CHECK(GetPartitions(*test_device, partitions) == FilesystemResultCodes::SUCCESS);
            CHECK_EQUAL(1, partitions.size());
        }

        void teardown()
        {
            test_device = minstd::unique_ptr<ut_utility::InMemoryFileBlockIODevice>();
            partitions.clear();

            CHECK_EQUAL(heap_bytes_at_start_, __os_dynamic_heap_core.bytes_in_use());
        }
    };
#pragma GCC diagnostic pop

    TEST(FAT32DirectoryClusterTest, IteratorHandlesAllEndOfChainMarkers)
    {
        //  In test_fat32.img the root directory chains cluster 2 -> cluster 12 -> EOF, and
        //      cluster 2 is full: 16 in-use entries, no 0x00 terminator - that sits at index 2
        //      of cluster 12.  Truncating the chain at cluster 2 removes the directory's only
        //      terminator, so iteration must rely entirely on end-of-chain detection.
        //
        //      0x0FFFFFF8 through 0x0FFFFFFF are all end-of-chain, so the marker VALUE must not
        //      change the result.  Asserting the four agree catches early termination as well
        //      as overrun, which ">= 0" does not.  0x0FFFFFFF supplies the baseline because it
        //      is the one value the original code already handled.

        const int32_t baseline = CountRootEntriesWithChainTerminator(0x0FFFFFFF);

        CHECK(baseline > 0);

        CHECK_EQUAL(baseline, CountRootEntriesWithChainTerminator(0x0FFFFFF8));
        CHECK_EQUAL(baseline, CountRootEntriesWithChainTerminator(0x0FFFFFFA));
        CHECK_EQUAL(baseline, CountRootEntriesWithChainTerminator(0x0FFFFFFC));
    }

    TEST(FAT32DirectoryClusterTest, CopiedIteratorDoesNotReadTheSourceBuffer)
    {
        auto test_fat32 = FAT32Filesystem::Mount(false, "test_fat32", "TESTFAT32", false, *test_device, partitions[0]);

        CHECK(test_fat32.Successful());

        const FAT32ClusterIndex first_cluster = test_fat32->BlockIOAdapter().RootDirectoryCluster();

        FAT32DirectoryCluster directory(test_fat32->Id(), test_fat32->BlockIOAdapter(), first_cluster);

        //  Position the original on the first entry, which loads cluster 2 into its buffer.

        auto original = directory.directory_entry_iterator_begin();

        auto first_entry = original.AsClusterEntry();

        CHECK(first_entry.Successful());

        const FAT32Compact8Dot3Filename expected_name = first_entry->CompactName();

        //  Copy it while it sits on that entry.

        auto copy = original;

        //  Walk the original into the root directory's second cluster (2 -> 12).  That reloads
        //      the ORIGINAL's buffer with cluster 12's contents.

        while (!original.end())
        {
            auto address = original.AsEntryAddress();

            CHECK(address.Successful());

            if (address->Cluster() != first_cluster)
            {
                break;
            }

            CHECK_EQUAL(FilesystemResultCodes::SUCCESS, original++);
        }

        CHECK_FALSE(original.end());

        //  The copy must still read its own entry.  Without the copy constructor its cached
        //      directory_entries_ pointer aims at the original's buffer, which now holds
        //      cluster 12, so this reads a different name.

        auto copied_entry = copy.AsClusterEntry();

        CHECK(copied_entry.Successful());
        CHECK(copied_entry->CompactName() == expected_name);
    }

    TEST(FAT32DirectoryClusterTest, RemoveEntryAtFirstIndexOfFirstClusterSucceeds)
    {
        auto test_fat32 = FAT32Filesystem::Mount(false, "test_fat32", "TESTFAT32", false, *test_device, partitions[0]);

        CHECK(test_fat32.Successful());

        FAT32ClusterIndex root_cluster = test_fat32->BlockIOAdapter().RootDirectoryCluster();

        FAT32DirectoryCluster directory(test_fat32->Id(), test_fat32->BlockIOAdapter(), root_cluster);

        //  Index 0 of the first cluster (the volume label) has nothing in front of it, so the
        //      backwards LFN scan has nowhere to go.  That is success.

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS,
                    directory.RemoveEntry(FAT32DirectoryEntryAddress(root_cluster, 0)));
    }

    TEST(FAT32DirectoryClusterTest, MoveToDirectoryRejectsNonDataClusters)
    {
        auto test_fat32 = FAT32Filesystem::Mount(false, "test_fat32", "TESTFAT32", false, *test_device, partitions[0]);

        CHECK(test_fat32.Successful());

        FAT32ClusterIndex root_cluster = test_fat32->BlockIOAdapter().RootDirectoryCluster();
        FAT32ClusterIndex max_cluster = test_fat32->BlockIOAdapter().MaximumClusterNumber();

        FAT32DirectoryCluster directory(test_fat32->Id(), test_fat32->BlockIOAdapter(), root_cluster);

        CHECK_EQUAL(FilesystemResultCodes::FAT32_CLUSTER_OUT_OF_RANGE, directory.MoveToDirectory(FAT32ClusterIndex(0)));
        CHECK_EQUAL(FilesystemResultCodes::FAT32_CLUSTER_OUT_OF_RANGE, directory.MoveToDirectory(FAT32ClusterIndex(1)));
        CHECK_EQUAL(FilesystemResultCodes::FAT32_CLUSTER_OUT_OF_RANGE, directory.MoveToDirectory(FAT32ClusterIndex((uint32_t)max_cluster + 1)));
        CHECK_EQUAL(FilesystemResultCodes::FAT32_CLUSTER_OUT_OF_RANGE, directory.MoveToDirectory(FAT32EntryAllocatedAndEndOfFile));

        //  SUBDIR1 is cluster 3.

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, directory.MoveToDirectory(FAT32ClusterIndex(3)));
    }

        TEST(FAT32DirectoryClusterTest, GetClusterEntryRejectsOutOfRangeIndex)
    {
        auto test_fat32 = FAT32Filesystem::Mount(false, "test_fat32", "TESTFAT32", false, *test_device, partitions[0]);

        CHECK(test_fat32.Successful());

        FAT32BlockIOAdapter &adapter = test_fat32->BlockIOAdapter();
        const FAT32ClusterIndex root_cluster = adapter.RootDirectoryCluster();
        const uint32_t entries_per_cluster = adapter.BytesPerCluster() / sizeof(FAT32DirectoryClusterEntry);

        FAT32DirectoryCluster directory(test_fat32->Id(), adapter, root_cluster);

        //  The last slot is legal; one past it is not.

        CHECK(directory.GetClusterEntry(FAT32DirectoryEntryAddress(root_cluster, entries_per_cluster - 1)).Successful());

        CHECK_FAILED_WITH_CODE(FilesystemResultCodes::FAT32_CURRENT_DIRECTORY_ENTRY_IS_INVALID,
                               directory.GetClusterEntry(FAT32DirectoryEntryAddress(root_cluster, entries_per_cluster)));
    }

    TEST(FAT32DirectoryClusterTest, EntryWritersRejectOutOfRangeIndex)
    {
        auto test_fat32 = FAT32Filesystem::Mount(false, "test_fat32", "TESTFAT32", false, *test_device, partitions[0]);

        CHECK(test_fat32.Successful());

        FAT32BlockIOAdapter &adapter = test_fat32->BlockIOAdapter();
        const FAT32ClusterIndex root_cluster = adapter.RootDirectoryCluster();
        const uint32_t entries_per_cluster = adapter.BytesPerCluster() / sizeof(FAT32DirectoryClusterEntry);
        const FAT32DirectoryEntryAddress past_end(root_cluster, entries_per_cluster);

        FAT32DirectoryCluster directory(test_fat32->Id(), adapter, root_cluster);

        CHECK_EQUAL(FilesystemResultCodes::FAT32_CURRENT_DIRECTORY_ENTRY_IS_INVALID, directory.RemoveEntry(past_end));
        CHECK_EQUAL(FilesystemResultCodes::FAT32_CURRENT_DIRECTORY_ENTRY_IS_INVALID,
                    FAT32Directory::SetDirectoryEntryFirstCluster(adapter, past_end, FAT32ClusterIndex(40)));
        CHECK_EQUAL(FilesystemResultCodes::FAT32_CURRENT_DIRECTORY_ENTRY_IS_INVALID,
                    FAT32Directory::UpdateDirectoryEntrySize(adapter, past_end, 0));
    }
}
