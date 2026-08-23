// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "../cpputest_support.h"

#include <__memory_resource/monotonic_buffer_resource.h>
#include <__memory_resource/polymorphic_allocator.h>

#include "../utility/in_memory_blockio_device.h"

#include "filesystem/master_boot_record.h"

namespace
{
    using namespace filesystems;
    using namespace filesystems::fat32;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (MasterBootRecord)
    {
    };
#pragma GCC diagnostic pop

    TEST(MasterBootRecord, LoadMBR)
    {
        //  Load the empty 32MB FAT32 image and insure the MBR is correct

        ut_utility::InMemoryFileBlockIODevice test_device("UNIT_TEST_EMPTY_FILE");

        CHECK(test_device.Open("./test/data/empty_fat32.img"));

        alignas(MassStoragePartition) uint8_t partition_buffer[sizeof(MassStoragePartition) * MAX_PARTITIONS_ON_MASS_STORAGE_DEVICE + alignof(MassStoragePartition) * MAX_PARTITIONS_ON_MASS_STORAGE_DEVICE];
        minstd::pmr::monotonic_buffer_resource partition_resource(partition_buffer, sizeof(partition_buffer), nullptr);
        minstd::pmr::polymorphic_allocator<MassStoragePartition> partition_allocator(&partition_resource);

        MassStoragePartitions partitions(partition_allocator);

        CHECK(GetPartitions(test_device, partitions) == FilesystemResultCodes::SUCCESS);
        CHECK_EQUAL(1, partitions.size());
        STRCMP_EQUAL("EMPTYFAT32", partitions[0].Name().c_str());
    }

    TEST(MasterBootRecord, LoadMBRDevicReadError)
    {
        //  Load the empty 32MB FAT32 image and insure the MBR is correct

        ut_utility::InMemoryFileBlockIODevice test_device("UNIT_TEST_EMPTY_FILE");

        CHECK(test_device.Open("./test/data/empty_fat32.img"));

        alignas(MassStoragePartition) uint8_t partition_buffer[sizeof(MassStoragePartition) * MAX_PARTITIONS_ON_MASS_STORAGE_DEVICE + alignof(MassStoragePartition) * MAX_PARTITIONS_ON_MASS_STORAGE_DEVICE];
        minstd::pmr::monotonic_buffer_resource partition_resource(partition_buffer, sizeof(partition_buffer), nullptr);
        minstd::pmr::polymorphic_allocator<MassStoragePartition> partition_allocator(&partition_resource);

        MassStoragePartitions partitions(partition_allocator);

        test_device.SimulateReadError();

        CHECK(GetPartitions(test_device, partitions) == FilesystemResultCodes::UNABLE_TO_READ_MASTER_BOOT_RECORD);
        //        CHECK_EQUAL(1, partitions.size());
        //        STRCMP_EQUAL("EMPTYFAT32", partitions[0].Name().c_str());
    }

    TEST(MasterBootRecord, LoadMBRBadMagic)
    {
        ut_utility::InMemoryFileBlockIODevice test_device("UNIT_TEST_BAD_MAGIC");

        CHECK(test_device.Open("./test/data/empty_fat32.img"));

        uint8_t mbr_buffer[ut_utility::InMemoryFileBlockIODevice::BLOCK_SIZE_IN_BYTES];
        CHECK(test_device.ReadFromBlock(mbr_buffer, 0, 1).Successful());

        mbr_buffer[510] = 0x00;
        mbr_buffer[511] = 0x00;

        CHECK(test_device.WriteBlock(mbr_buffer, 0, 1).Successful());

        alignas(MassStoragePartition) uint8_t partition_buffer[sizeof(MassStoragePartition) * MAX_PARTITIONS_ON_MASS_STORAGE_DEVICE + alignof(MassStoragePartition) * MAX_PARTITIONS_ON_MASS_STORAGE_DEVICE];
        minstd::pmr::monotonic_buffer_resource partition_resource(partition_buffer, sizeof(partition_buffer), nullptr);
        minstd::pmr::polymorphic_allocator<MassStoragePartition> partition_allocator(&partition_resource);

        MassStoragePartitions partitions(partition_allocator);

        CHECK(GetPartitions(test_device, partitions) == FilesystemResultCodes::BAD_MASTER_BOOT_RECORD_MAGIC_NUMBER);
    }

    TEST(MasterBootRecord, LoadMBRUnsupportedPartitionType)
    {
        ut_utility::InMemoryFileBlockIODevice test_device("UNIT_TEST_NOT_FAT32");

        CHECK(test_device.Open("./test/data/empty_fat32.img"));

        uint8_t mbr_buffer[ut_utility::InMemoryFileBlockIODevice::BLOCK_SIZE_IN_BYTES];
        CHECK(test_device.ReadFromBlock(mbr_buffer, 0, 1).Successful());

        // Change the first partition type to a non-FAT32 value.
        mbr_buffer[0x1BE + 4] = 0x01;

        CHECK(test_device.WriteBlock(mbr_buffer, 0, 1).Successful());

        alignas(MassStoragePartition) uint8_t partition_buffer[sizeof(MassStoragePartition) * MAX_PARTITIONS_ON_MASS_STORAGE_DEVICE + alignof(MassStoragePartition) * MAX_PARTITIONS_ON_MASS_STORAGE_DEVICE];
        minstd::pmr::monotonic_buffer_resource partition_resource(partition_buffer, sizeof(partition_buffer), nullptr);
        minstd::pmr::polymorphic_allocator<MassStoragePartition> partition_allocator(&partition_resource);

        MassStoragePartitions partitions(partition_allocator);

        CHECK(GetPartitions(test_device, partitions) == FilesystemResultCodes::SUCCESS);
        CHECK_EQUAL(0, partitions.size());
    }
}