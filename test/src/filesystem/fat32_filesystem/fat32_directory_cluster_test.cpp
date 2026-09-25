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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (FAT32DirectoryClusterTest)
    {
        void setup()
        {
            CHECK_EQUAL(0, __os_dynamic_heap_core.bytes_in_use());

            test_device = make_dynamic_unique<ut_utility::InMemoryFileBlockIODevice>("IN_MEMORY_TEST_DEVICE");

            CHECK(test_device->Open("./test/data/test_fat32.img"));
            CHECK(GetPartitions(*test_device, partitions) == FilesystemResultCodes::SUCCESS);
            CHECK_EQUAL(1, partitions.size());
        }

        void teardown()
        {
            test_device = minstd::unique_ptr<ut_utility::InMemoryFileBlockIODevice>();
            partitions.clear();

            CHECK_EQUAL(0, __os_dynamic_heap_core.bytes_in_use());
        }
    };
#pragma GCC diagnostic pop

    TEST(FAT32DirectoryClusterTest, IteratorHandlesAllEndOfChainMarkers)
    {
        //  mkfs.fat writes 0x0FFFFFF8 routinely, but AdvanceCurrentEntry() recognised only an
        //      exact 0x0FFFFFFF.  Anything else was followed as if it were a cluster number.

        const uint32_t markers[] = {0x0FFFFFF8, 0x0FFFFFFA, 0x0FFFFFFC, 0x0FFFFFFF};

        for (uint32_t m = 0; m < (sizeof(markers) / sizeof(markers[0])); m++)
        {
            auto test_fat32 = FAT32Filesystem::Mount(false, "test_fat32", "TESTFAT32", false, *test_device, partitions[0]);

            CHECK(test_fat32.Successful());

            FAT32ClusterIndex root_cluster = test_fat32->BlockIOAdapter().RootDirectoryCluster();

            CHECK_EQUAL(FilesystemResultCodes::SUCCESS,
                        test_fat32->BlockIOAdapter().UpdateFATTableEntry(root_cluster, FAT32ClusterIndex(markers[m])));

            FAT32DirectoryCluster directory(test_fat32->Id(), test_fat32->BlockIOAdapter(), root_cluster);

            CHECK(CountEntries(directory) >= 0);
        }
    }

}