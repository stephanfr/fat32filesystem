// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include <__memory_resource/monotonic_buffer_resource.h>
#include <__memory_resource/polymorphic_allocator.h>

#include "in_memory_blockio_device.h"

#include "filesystem/fat32_filesystem.h"
#include "filesystem/fat32_partition.h"
#include "filesystem/master_boot_record.h"

namespace ut_utility
{
    //
    //  Mounts a FAT32 image and registers the filesystem with the OS entity registry.
    //
    //  Registration is what makes FAT32Directory and FAT32File testable at all - every one of
    //      their entry points opens with GetOSEntityRegistry().GetEntityById(FilesystemUUID())
    //      and bails out with FILESYSTEM_DOES_NOT_EXIST if the lookup fails.
    //
    class MountedTestFilesystem
    {
    public:
        MountedTestFilesystem() = default;

        ~MountedTestFilesystem()
        {
            Unmount();
        }

        //  Returns false if the image could not be opened, partitioned or mounted.

        bool Mount(const char *image_path = "./test/data/test_fat32.img")
        {
            device_ = make_dynamic_unique<InMemoryFileBlockIODevice>("IN_MEMORY_TEST_DEVICE");

            if (!device_->Open(image_path))
            {
                return false;
            }

            if (filesystems::GetPartitions(*device_, partitions_) != filesystems::FilesystemResultCodes::SUCCESS)
            {
                return false;
            }

            if (partitions_.size() != 1)
            {
                return false;
            }

            auto mounted = filesystems::fat32::FAT32Filesystem::Mount(false,
                                                                      "test_fat32",
                                                                      "TESTFAT32",
                                                                      false,
                                                                      *device_,
                                                                      partitions_[0]);

            if (!mounted.Successful())
            {
                return false;
            }

            filesystem_ = mounted.Value().get();
            filesystem_uuid_ = mounted->Id();

            return Successful(GetOSEntityRegistry().AddEntity(mounted.Value()));
        }

        void Unmount()
        {
            if (filesystem_ != nullptr)
            {
                //  VERIFY: registry removal method name.
                GetOSEntityRegistry().RemoveEntityById(filesystem_uuid_);
                filesystem_ = nullptr;
            }

            partitions_.clear();
            device_ = minstd::unique_ptr<InMemoryFileBlockIODevice>();
        }

        InMemoryFileBlockIODevice &Device() { return *device_; }
        filesystems::fat32::FAT32Filesystem &Filesystem() { return *filesystem_; }
        const UUID &FilesystemUUID() const { return filesystem_uuid_; }

        filesystems::fat32::FAT32BlockIOAdapter &Adapter() { return filesystem_->BlockIOAdapter(); }

        //  Convenience: the root directory, already unwrapped.

        minstd::unique_ptr<filesystems::FilesystemDirectory> RootDirectory()
        {
            auto root = filesystem_->GetRootDirectory();

            CHECK(root.Successful());

            return minstd::move(root.Value());
        }

        //  Raw cluster access, for tests that assert on the on-disk bytes directly.

        bool ReadRawCluster(filesystems::fat32::FAT32ClusterIndex cluster, uint8_t *buffer)
        {
            return Adapter().ReadCluster(cluster, buffer) == BlockIOResultCodes::SUCCESS;
        }

    private:
        static constexpr size_t PARTITION_BUFFER_SIZE =
            (sizeof(filesystems::MassStoragePartition) + alignof(filesystems::MassStoragePartition)) *
            MAX_PARTITIONS_ON_MASS_STORAGE_DEVICE;

        minstd::unique_ptr<InMemoryFileBlockIODevice> device_;

        alignas(filesystems::MassStoragePartition) uint8_t partition_buffer_[PARTITION_BUFFER_SIZE];
        minstd::pmr::monotonic_buffer_resource partition_resource_{partition_buffer_, sizeof(partition_buffer_), nullptr};
        minstd::pmr::polymorphic_allocator<filesystems::MassStoragePartition> partition_allocator_{&partition_resource_};

        filesystems::MassStoragePartitions partitions_{partition_allocator_};

        filesystems::fat32::FAT32Filesystem *filesystem_ = nullptr;
        UUID filesystem_uuid_ = UUID::NIL;
    };
}
