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

    constexpr uint64_t TEST_SEED = 1;

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

    //  A directory as the cache records it.  These tests only look at its first cluster.

    FAT32DirectoryCacheEntry Directory(uint32_t first_cluster)
    {
        return FAT32DirectoryCacheEntry(FAT32DirectoryEntryAddress(FAT32ClusterIndex(2), first_cluster),
                                        FAT32ClusterIndex(first_cluster),
                                        FAT32Compact8Dot3Filename("DIR", ""));
    }

    void Add(FAT32DirectoryCache &cache, uint32_t parent, const char *name, uint32_t first_cluster)
    {
        cache.Add(FAT32ClusterIndex(parent), name, Directory(first_cluster));
    }

    bool IsCached(FAT32DirectoryCache &cache, uint32_t parent, const char *name)
    {
        return cache.Find(FAT32ClusterIndex(parent), name).has_value();
    }

    bool Resolves(FAT32DirectoryCache &cache, uint32_t parent, const char *name, uint32_t first_cluster)
    {
        auto found = cache.Find(FAT32ClusterIndex(parent), name);

        return found.has_value() && (found->FirstClusterId() == FAT32ClusterIndex(first_cluster));
    }

    //  ... TEST_GROUP (FAT32DirectoryCache) unchanged ...

    TEST(FAT32DirectoryCache, FindsWhatWasAdded)
    {
        FAT32DirectoryCache cache(16, TEST_SEED);

        CHECK(!IsCached(cache, 2, "Documents"));

        Add(cache, 2, "Documents", 10);

        CHECK(Resolves(cache, 2, "Documents", 10));
        CHECK(cache.Size() == 1);
        CHECK(cache.Hits() == 1);
        CHECK(cache.Misses() == 1);
    }

    TEST(FAT32DirectoryCache, NamesCompareWithoutCase)
    {
        FAT32DirectoryCache cache(16, TEST_SEED);

        Add(cache, 2, "Documents", 10);

        CHECK(Resolves(cache, 2, "DOCUMENTS", 10));
        CHECK(Resolves(cache, 2, "documents", 10));
        CHECK(!IsCached(cache, 2, "Document"));
        CHECK(!IsCached(cache, 2, "Documents2"));
    }

    TEST(FAT32DirectoryCache, TheSameNameInDifferentDirectoriesIsADifferentEntry)
    {
        FAT32DirectoryCache cache(16, TEST_SEED);

        Add(cache, 2, "DATA", 10);
        Add(cache, 10, "DATA", 11);

        CHECK(Resolves(cache, 2, "DATA", 10));
        CHECK(Resolves(cache, 10, "DATA", 11));
        CHECK(!IsCached(cache, 11, "DATA"));
    }

    TEST(FAT32DirectoryCache, ReAddingANameReplacesItsEntry)
    {
        FAT32DirectoryCache cache(16, TEST_SEED);

        Add(cache, 2, "DATA", 10);
        Add(cache, 2, "data", 10);

        CHECK(cache.Size() == 1);
    }

    TEST(FAT32DirectoryCache, AFullCacheForgetsTheLeastRecentlyUsedName)
    {
        FAT32DirectoryCache cache(2, TEST_SEED);

        Add(cache, 2, "A", 10);
        Add(cache, 2, "B", 11);

        CHECK(IsCached(cache, 2, "A"));     //  A is now the most recently used

        Add(cache, 2, "C", 12);

        CHECK(cache.Size() == 2);
        CHECK(Resolves(cache, 2, "A", 10));
        CHECK(!IsCached(cache, 2, "B"));
        CHECK(Resolves(cache, 2, "C", 12));
    }

    TEST(FAT32DirectoryCache, RemovedSlotsAreReusedBeforeAnythingIsEvicted)
    {
        FAT32DirectoryCache cache(2, TEST_SEED);

        Add(cache, 2, "A", 10);
        Add(cache, 2, "B", 11);

        cache.Remove(FAT32ClusterIndex(10));

        Add(cache, 2, "C", 12);

        CHECK(cache.Size() == 2);
        CHECK(Resolves(cache, 2, "B", 11));
        CHECK(Resolves(cache, 2, "C", 12));
    }

    TEST(FAT32DirectoryCache, RemoveForgetsEveryNameForADirectoryButNotItsChildren)
    {
        FAT32DirectoryCache cache(16, TEST_SEED);

        Add(cache, 2, "Long Directory Name", 10);
        Add(cache, 2, "LONGDI~1", 10);
        Add(cache, 10, "CHILD", 11);

        cache.Remove(FAT32ClusterIndex(10));

        CHECK(!IsCached(cache, 2, "Long Directory Name"));
        CHECK(!IsCached(cache, 2, "LONGDI~1"));
        CHECK(Resolves(cache, 10, "CHILD", 11));
        CHECK(cache.Size() == 1);
    }

    TEST(FAT32DirectoryCache, RemoveWithChildrenAlsoForgetsNamesInsideTheDirectory)
    {
        FAT32DirectoryCache cache(16, TEST_SEED);

        Add(cache, 2, "Long Directory Name", 10);
        Add(cache, 2, "LONGDI~1", 10);
        Add(cache, 10, "CHILD", 11);
        Add(cache, 2, "OTHER", 12);

        cache.RemoveWithChildren(FAT32ClusterIndex(10));

        CHECK(!IsCached(cache, 2, "Long Directory Name"));
        CHECK(!IsCached(cache, 2, "LONGDI~1"));
        CHECK(!IsCached(cache, 10, "CHILD"));
        CHECK(Resolves(cache, 2, "OTHER", 12));
        CHECK(cache.Size() == 1);
    }

    TEST(FAT32DirectoryCache, OnlyLegalFAT32NamesAreCached)
    {
        FAT32DirectoryCache cache(16, TEST_SEED);

        char name[MAX_FILENAME_LENGTH + 2];

        for (size_t i = 0; i <= MAX_FILENAME_LENGTH; i++)
        {
            name[i] = 'A';
        }

        name[MAX_FILENAME_LENGTH + 1] = 0;      //  256 characters - too long

        Add(cache, 2, name, 10);

        CHECK(cache.Size() == 0);
        CHECK(!IsCached(cache, 2, name));

        name[MAX_FILENAME_LENGTH] = 0;          //  255 characters - the longest legal name

        Add(cache, 2, name, 10);

        CHECK(Resolves(cache, 2, name, 10));

        Add(cache, 2, "", 11);

        CHECK(cache.Size() == 1);
    }

    TEST(FAT32DirectoryCache, AllocatesNothingAfterConstruction)
    {
        FAT32DirectoryCache cache(8, TEST_SEED);

        //  The tests map the filesystem cache heap onto the dynamic heap.

        const size_t heap_bytes_after_construction = __os_dynamic_heap_core.bytes_in_use();

        char name[] = "DIR000";

        for (uint32_t i = 0; i < 100; i++)
        {
            name[3] = '0' + (i / 100);
            name[4] = '0' + ((i / 10) % 10);
            name[5] = '0' + (i % 10);

            Add(cache, 2, name, 100 + i);

            CHECK(IsCached(cache, 2, name));
        }

        CHECK(cache.Size() == 8);

        cache.RemoveWithChildren(FAT32ClusterIndex(195));
        cache.Clear();

        CHECK(cache.Size() == 0);
        CHECK(heap_bytes_after_construction == __os_dynamic_heap_core.bytes_in_use());
    }

    TEST(FAT32DirectoryCache, IsDisabledWhenItsMemoryCannotBeAllocated)
    {
        //  MAX_CAPACITY entries need more than the 256 MB test heap holds.

        FAT32DirectoryCache cache(FAT32DirectoryCache::MAX_CAPACITY, TEST_SEED);

        CHECK(cache.Capacity() == 0);

        Add(cache, 2, "A", 10);

        CHECK(!IsCached(cache, 2, "A"));
        CHECK(cache.Size() == 0);
    }

    TEST(FAT32DirectoryCache, ACapacityOfZeroDisablesTheCache)
    {
        FAT32DirectoryCache cache(0, TEST_SEED);

        CHECK(cache.Capacity() == 0);

        Add(cache, 2, "A", 10);

        CHECK(!IsCached(cache, 2, "A"));
    }
}
