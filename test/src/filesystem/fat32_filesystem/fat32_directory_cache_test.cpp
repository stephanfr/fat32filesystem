// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "../../cpputest_support.h"

#include <minimalcstdlib.h>

#include "filesystem/fat32_filesystem.h"

namespace
{
    using namespace filesystems;
    using namespace filesystems::fat32;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (FAT32DirectoryCache)
    {
        size_t heap_bytes_at_start_ = 0;

        void setup()
        {
            heap_bytes_at_start_ = __os_dynamic_heap_core.bytes_in_use();
            LogInfo("Setup: Heap Bytes Allocated: %d\n", heap_bytes_at_start_);
        }

        void teardown()
        {
            LogInfo("Teardown: Heap Bytes Allocated: %d\n", __os_dynamic_heap_core.bytes_in_use());
            CHECK_EQUAL(heap_bytes_at_start_, __os_dynamic_heap_core.bytes_in_use());
        }
    };
#pragma GCC diagnostic pop

    //
    //  Tests start below
    //

    TEST(FAT32DirectoryCache, BasicTest)
    {
        FAT32DirectoryCache directory_cache(1024);

        directory_cache.AddEntry(FAT32DirectoryCacheEntryType::DIRECTORY, FAT32DirectoryEntryAddress(FAT32ClusterIndex(1), 1), FAT32ClusterIndex(1), FAT32Compact8Dot3Filename("director", "y1"), minstd::fixed_string<>("directory1"));
        directory_cache.AddEntry(FAT32DirectoryCacheEntryType::DIRECTORY, FAT32DirectoryEntryAddress(FAT32ClusterIndex(2), 2), FAT32ClusterIndex(2), FAT32Compact8Dot3Filename("director", "y2"), minstd::fixed_string<>("directory2"));
        directory_cache.AddEntry(FAT32DirectoryCacheEntryType::DIRECTORY, FAT32DirectoryEntryAddress(FAT32ClusterIndex(3), 3), FAT32ClusterIndex(3), FAT32Compact8Dot3Filename("director", "y3"), minstd::fixed_string<>("directory3"));

        {
            auto entry1 = directory_cache.FindEntry(FAT32ClusterIndex(1));
            CHECK(entry1.has_value());
            CHECK_EQUAL((uint32_t)FAT32DirectoryCacheEntryType::DIRECTORY, (uint32_t)entry1->get().EntryType());
            CHECK_EQUAL(1, (uint32_t)entry1->get().FirstClusterId());
            STRCMP_EQUAL("directory1", entry1->get().AbsolutePath().c_str());

            auto index1 = directory_cache.FindFirstClusterIndex(minstd::fixed_string<>("directory1"));
            CHECK_EQUAL(1, (uint32_t)index1.value());
        }

        {
            auto entry2 = directory_cache.FindEntry(FAT32ClusterIndex(2));
            CHECK(entry2.has_value());
            CHECK_EQUAL((uint32_t)FAT32DirectoryCacheEntryType::DIRECTORY, (uint32_t)entry2->get().EntryType());
            CHECK_EQUAL(2, (uint32_t)entry2->get().FirstClusterId());
            STRCMP_EQUAL("directory2", entry2->get().AbsolutePath().c_str());

            auto index2 = directory_cache.FindFirstClusterIndex(minstd::fixed_string<>("directory2"));
            CHECK_EQUAL(2, (uint32_t)index2.value());
        }

        {
            auto entry3 = directory_cache.FindEntry(FAT32ClusterIndex(3));
            CHECK(entry3.has_value());
            CHECK_EQUAL((uint32_t)FAT32DirectoryCacheEntryType::DIRECTORY, (uint32_t)entry3->get().EntryType());
            CHECK_EQUAL(3, (uint32_t)entry3->get().FirstClusterId());
            STRCMP_EQUAL("directory3", entry3->get().AbsolutePath().c_str());

            auto index3 = directory_cache.FindFirstClusterIndex(minstd::fixed_string<>("directory3"));
            CHECK_EQUAL(3, (uint32_t)index3.value());
        }

        directory_cache.RemoveEntry(FAT32ClusterIndex(2));

        CHECK(directory_cache.FindEntry(FAT32ClusterIndex(2)).has_value() == false);
        CHECK(directory_cache.FindFirstClusterIndex(minstd::fixed_string<>("directory2")).has_value() == false);

        {
            auto entry1 = directory_cache.FindEntry(FAT32ClusterIndex(1));
            CHECK(entry1.has_value());
            CHECK_EQUAL((uint32_t)FAT32DirectoryCacheEntryType::DIRECTORY, (uint32_t)entry1->get().EntryType());
            CHECK_EQUAL(1, (uint32_t)entry1->get().FirstClusterId());
            STRCMP_EQUAL("directory1", entry1->get().AbsolutePath().c_str());

            auto index1 = directory_cache.FindFirstClusterIndex(minstd::fixed_string<>("directory1"));
            CHECK_EQUAL(1, (uint32_t)index1.value());
        }

        {
            auto entry3 = directory_cache.FindEntry(FAT32ClusterIndex(3));
            CHECK(entry3.has_value());
            CHECK_EQUAL((uint32_t)FAT32DirectoryCacheEntryType::DIRECTORY, (uint32_t)entry3->get().EntryType());
            CHECK_EQUAL(3, (uint32_t)entry3->get().FirstClusterId());
            STRCMP_EQUAL("directory3", entry3->get().AbsolutePath().c_str());

            auto index3 = directory_cache.FindFirstClusterIndex(minstd::fixed_string<>("directory3"));
            CHECK_EQUAL(3, (uint32_t)index3.value());
        }

        directory_cache.RemoveEntry(FAT32ClusterIndex(3));
        CHECK(directory_cache.FindEntry(FAT32ClusterIndex(3)).has_value() == false);
        CHECK(directory_cache.FindFirstClusterIndex(minstd::fixed_string<>("directory3")).has_value() == false);

        {
            auto entry1 = directory_cache.FindEntry(FAT32ClusterIndex(1));
            CHECK(entry1.has_value());
            CHECK_EQUAL((uint32_t)FAT32DirectoryCacheEntryType::DIRECTORY, (uint32_t)entry1->get().EntryType());
            CHECK_EQUAL(1, (uint32_t)entry1->get().FirstClusterId());
            STRCMP_EQUAL("directory1", entry1->get().AbsolutePath().c_str());

            auto index1 = directory_cache.FindFirstClusterIndex(minstd::fixed_string<>("directory1"));
            CHECK_EQUAL(1, (uint32_t)index1.value());
        }

        directory_cache.RemoveEntry(FAT32ClusterIndex(1));
        CHECK(directory_cache.FindEntry(FAT32ClusterIndex(1)).has_value() == false);
        CHECK(directory_cache.FindFirstClusterIndex(minstd::fixed_string<>("directory1")).has_value() == false);
    }

    TEST(FAT32DirectoryCache, NegativeTests)
    {
        FAT32DirectoryCache directory_cache(1024);

        directory_cache.AddEntry(FAT32DirectoryCacheEntryType::DIRECTORY, FAT32DirectoryEntryAddress(FAT32ClusterIndex(1), 1), FAT32ClusterIndex(1), FAT32Compact8Dot3Filename("director", "y1"), minstd::fixed_string<>("directory1"));
        CHECK(directory_cache.FindEntry(FAT32ClusterIndex(1)).has_value());
        CHECK(directory_cache.FindFirstClusterIndex(minstd::fixed_string<>("directory1")).has_value());

        //  Simulate a path has collision by adding a new entry with a different cluster index but the same path

        directory_cache.AddEntry(FAT32DirectoryCacheEntryType::DIRECTORY, FAT32DirectoryEntryAddress(FAT32ClusterIndex(2), 2), FAT32ClusterIndex(2), FAT32Compact8Dot3Filename("director", "y1"), minstd::fixed_string<>("directory1"));
        CHECK(directory_cache.FindEntry(FAT32ClusterIndex(1)).has_value());
        CHECK(directory_cache.FindFirstClusterIndex(minstd::fixed_string<>("directory1")).has_value());
        CHECK(!directory_cache.FindEntry(FAT32ClusterIndex(2)).has_value());
        CHECK(!directory_cache.FindFirstClusterIndex(minstd::fixed_string<>("directory2")).has_value());

        //  Re-inserting the first entry should be a no-op

        directory_cache.AddEntry(FAT32DirectoryCacheEntryType::DIRECTORY, FAT32DirectoryEntryAddress(FAT32ClusterIndex(1), 1), FAT32ClusterIndex(1), FAT32Compact8Dot3Filename("director", "y1"), minstd::fixed_string<>("directory1"));

        //  Removing a non-existant entry should be a no-op

        directory_cache.RemoveEntry(FAT32ClusterIndex(4));
    }

    TEST(FAT32DirectoryCache, SizeAndClearTest)
    {
        FAT32DirectoryCache directory_cache(10);
        char buffer[14] = "directory";
        char compact_extension[4] = "y  ";

        for (int i = 1; i <= 12; i++)
        {
            itoa(i, buffer + 9, 10);
            itoa(i, compact_extension + 1, 10);
            directory_cache.AddEntry(FAT32DirectoryCacheEntryType::DIRECTORY, FAT32DirectoryEntryAddress(FAT32ClusterIndex(i), i), FAT32ClusterIndex(i), FAT32Compact8Dot3Filename("director", compact_extension), minstd::fixed_string<>(buffer));
        }

        CHECK(directory_cache.MaxSize() == 10);
        CHECK(directory_cache.CurrentSize() == 10);
        CHECK(!directory_cache.FindEntry(FAT32ClusterIndex(1)).has_value());
        CHECK(!directory_cache.FindEntry(FAT32ClusterIndex(2)).has_value());
        CHECK(directory_cache.FindEntry(FAT32ClusterIndex(3)).has_value());

        directory_cache.Clear();

        CHECK(directory_cache.CurrentSize() == 0);
    }

    TEST(FAT32DirectoryCache, EvictedPathCanBeReCached)
    {
        constexpr size_t CACHE_SIZE = 2;

        FAT32DirectoryCache directory_cache(CACHE_SIZE, MurmurHash64ASeed(1));

        minstd::fixed_string<> first_path("/first");

        directory_cache.AddEntry(FAT32DirectoryCacheEntryType::DIRECTORY,
                                 FAT32DirectoryEntryAddress(FAT32ClusterIndex(2), 0),
                                 FAT32ClusterIndex(10),
                                 FAT32Compact8Dot3Filename("FIRST", ""),
                                 first_path);

        CHECK(directory_cache.FindEntry(FAT32ClusterIndex(10)).has_value());

        //  Push /first out of the cache.

        directory_cache.AddEntry(FAT32DirectoryCacheEntryType::DIRECTORY,
                                 FAT32DirectoryEntryAddress(FAT32ClusterIndex(2), 1),
                                 FAT32ClusterIndex(11),
                                 FAT32Compact8Dot3Filename("SECOND", ""),
                                 minstd::fixed_string<>("/second"));

        directory_cache.AddEntry(FAT32DirectoryCacheEntryType::DIRECTORY,
                                 FAT32DirectoryEntryAddress(FAT32ClusterIndex(2), 2),
                                 FAT32ClusterIndex(12),
                                 FAT32Compact8Dot3Filename("THIRD", ""),
                                 minstd::fixed_string<>("/third"));

        CHECK_FALSE(directory_cache.FindEntry(FAT32ClusterIndex(10)).has_value());

        //  Re-add it.  Pre-fix the stale path-hash entry makes AddEntry early-return, so
        //      nothing is inserted and /first can never re-enter the cache for the lifetime
        //      of the filesystem.

        directory_cache.AddEntry(FAT32DirectoryCacheEntryType::DIRECTORY,
                                 FAT32DirectoryEntryAddress(FAT32ClusterIndex(2), 0),
                                 FAT32ClusterIndex(10),
                                 FAT32Compact8Dot3Filename("FIRST", ""),
                                 first_path);

        CHECK(directory_cache.FindEntry(FAT32ClusterIndex(10)).has_value());
    }

    TEST(FAT32DirectoryCache, ReAddingTheSameEntryIsNotCountedAsACollision)
    {
        FAT32DirectoryCache directory_cache(16, MurmurHash64ASeed(1));

        minstd::fixed_string<> path("/same");

        directory_cache.AddEntry(FAT32DirectoryCacheEntryType::DIRECTORY,
                                 FAT32DirectoryEntryAddress(FAT32ClusterIndex(2), 0),
                                 FAT32ClusterIndex(20),
                                 FAT32Compact8Dot3Filename("SAME", ""),
                                 path);

        const uint64_t collisions_before = directory_cache.Collisions();

        directory_cache.AddEntry(FAT32DirectoryCacheEntryType::DIRECTORY,
                                 FAT32DirectoryEntryAddress(FAT32ClusterIndex(2), 0),
                                 FAT32ClusterIndex(20),
                                 FAT32Compact8Dot3Filename("SAME", ""),
                                 path);

        CHECK_EQUAL(collisions_before, directory_cache.Collisions());

        //  A genuinely different cluster reaching the same hash still counts.

        directory_cache.AddEntry(FAT32DirectoryCacheEntryType::DIRECTORY,
                                 FAT32DirectoryEntryAddress(FAT32ClusterIndex(2), 1),
                                 FAT32ClusterIndex(21),
                                 FAT32Compact8Dot3Filename("OTHER", ""),
                                 path);

        CHECK_EQUAL(collisions_before + 1, directory_cache.Collisions());
    }

}
