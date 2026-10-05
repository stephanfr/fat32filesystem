// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include <dynamic_string>
#include <minimalcstdlib.h>
#include <optional>

#include "os_config.h"
#include "platform/platform_sw_rngs.h"

#include "heaps.h"

#include "filesystem/fat32_directory.h"

namespace filesystems::fat32
{
    //  What the cache knows about one directory: where its entry is, where its contents start, and
    //      its 8.3 name.

    class FAT32DirectoryCacheEntry
    {
    public:
        FAT32DirectoryCacheEntry() = default;

        FAT32DirectoryCacheEntry(const FAT32DirectoryEntryAddress &entry_address,
                                 FAT32ClusterIndex first_cluster_id,
                                 const FAT32Compact8Dot3Filename &compact_name)
            : entry_address_(entry_address),
              first_cluster_id_(first_cluster_id),
              compact_name_(compact_name)
        {
        }

        const FAT32DirectoryEntryAddress &EntryAddress() const noexcept
        {
            return entry_address_;
        }

        FAT32ClusterIndex FirstClusterId() const noexcept
        {
            return first_cluster_id_;
        }

        const FAT32Compact8Dot3Filename &CompactName() const noexcept
        {
            return compact_name_;
        }

    private:
        FAT32DirectoryEntryAddress entry_address_;
        FAT32ClusterIndex first_cluster_id_ = FAT32ClusterIndex(0);
        FAT32Compact8Dot3Filename compact_name_;
    };

    //  A fixed-size, least-recently-used cache of directory lookups, one per mounted volume.
    //
    //  An entry records one name inside one directory - (the parent's first cluster, a name) - and
    //      the directory that name refers to.  Paths resolve one name at a time, so no paths are
    //      stored and path length does not matter.  A rename only makes the renamed directory's own
    //      names stale: everything below it is cached under its first cluster, which does not change.
    //
    //  All of the cache's memory is taken from the filesystem cache heap in a single allocation when
    //      the volume is mounted, and nothing is allocated after that - a full cache reuses its least
    //      recently used slot.  If that one allocation fails the cache is disabled, and every lookup
    //      reads the disk.
    //
    //  Names compare case-insensitively, as FAT names do.  A directory may be cached under its long
    //      name and its 8.3 alias; Remove() forgets both.
    //
    //  Not thread-safe: callers hold the filesystem lock.

    class FAT32DirectoryCache
    {
    public:
        //  Bounds the one allocation.  A larger capacity is refused and leaves the cache disabled.

        static constexpr size_t MAX_CAPACITY = 1u << 20;

        //  The bytes a cache of this many entries takes from the filesystem cache heap.

        static constexpr size_t BytesForCapacity(size_t capacity) noexcept
        {
            return (capacity * sizeof(Slot)) + (BucketCountFor(capacity) * sizeof(uint32_t));
        }

        explicit FAT32DirectoryCache(size_t capacity, uint64_t seed = GetGeneralRNG()())
            : seed_(seed)
        {
            if ((capacity == 0) || (capacity > MAX_CAPACITY))
            {
                return;
            }

            void *memory = __os_filesystem_cache_heap_resource.allocate(BytesForCapacity(capacity), alignof(Slot));

            if (memory == nullptr)
            {
                return;
            }

            memory_ = memory;
            capacity_ = capacity;
            bucket_mask_ = BucketCountFor(capacity) - 1;

            slots_ = static_cast<Slot *>(memory);
            buckets_ = reinterpret_cast<uint32_t *>(slots_ + capacity);

            for (size_t i = 0; i < capacity_; i++)
            {
                new (&slots_[i]) Slot();
            }

            Clear();
        }

        ~FAT32DirectoryCache()
        {
            if (memory_ != nullptr)
            {
                __os_filesystem_cache_heap_resource.deallocate(memory_, BytesForCapacity(capacity_), alignof(Slot));
            }
        }

        FAT32DirectoryCache(const FAT32DirectoryCache &) = delete;
        FAT32DirectoryCache &operator=(const FAT32DirectoryCache &) = delete;

        size_t Capacity() const noexcept
        {
            return capacity_;
        }

        size_t Size() const noexcept
        {
            return size_;
        }

        uint64_t Hits() const noexcept
        {
            return hits_;
        }

        uint64_t Misses() const noexcept
        {
            return misses_;
        }

        void Clear() noexcept
        {
            if (capacity_ == 0)
            {
                return;
            }

            for (size_t i = 0; i <= bucket_mask_; i++)
            {
                buckets_[i] = NONE;
            }

            //  Every slot goes on the free list, which is chained through older_.

            for (size_t i = 0; i < capacity_; i++)
            {
                slots_[i].older_ = ((i + 1) < capacity_) ? static_cast<uint32_t>(i + 1) : NONE;
            }

            free_ = 0;
            most_recent_ = NONE;
            least_recent_ = NONE;
            size_ = 0;
        }

        //  The directory 'name' refers to inside the directory whose first cluster is 'parent', if it is cached.

        minstd::optional<FAT32DirectoryCacheEntry> Find(FAT32ClusterIndex parent, const char *name) noexcept
        {
            const uint32_t slot = Locate(parent, name, strnlen(name, MAX_FILENAME_LENGTH + 1));

            if (slot == NONE)
            {
                misses_++;

                return minstd::optional<FAT32DirectoryCacheEntry>();
            }

            hits_++;

            MakeMostRecent(slot);

            return minstd::optional<FAT32DirectoryCacheEntry>(slots_[slot].directory_);
        }

        //  Records that 'name', inside the directory whose first cluster is 'parent', is 'directory'.
        //      A name that cannot be a FAT32 filename - empty, or over 255 characters - is not cached.

        void Add(FAT32ClusterIndex parent, const char *name, const FAT32DirectoryCacheEntry &directory) noexcept
        {
            const size_t length = strnlen(name, MAX_FILENAME_LENGTH + 1);

            if ((capacity_ == 0) || (length == 0) || (length > MAX_FILENAME_LENGTH))
            {
                return;
            }

            uint32_t slot = Locate(parent, name, length);

            if (slot != NONE)
            {
                slots_[slot].directory_ = directory;

                MakeMostRecent(slot);

                return;
            }

            //  Take a free slot, or reuse the least recently used one.

            if (free_ == NONE)
            {
                Release(least_recent_);
            }

            slot = free_;
            free_ = slots_[slot].older_;

            Slot &entry = slots_[slot];

            entry.hash_ = Hash(parent, name, length);
            entry.parent_ = parent;
            entry.directory_ = directory;
            entry.name_length_ = static_cast<uint16_t>(length);

            for (size_t i = 0; i < length; i++)
            {
                entry.name_[i] = name[i];
            }

            const size_t bucket = entry.hash_ & bucket_mask_;

            entry.next_in_bucket_ = buckets_[bucket];
            buckets_[bucket] = slot;

            LinkAsMostRecent(slot);

            size_++;
        }

        //  Forgets every name 'directory' is cached under - its long name and its 8.3 alias.  For a rename.

        void Remove(FAT32ClusterIndex directory) noexcept
        {
            ReleaseIf([directory](const Slot &slot)
                      { return slot.directory_.FirstClusterId() == directory; });
        }

        //  Also forgets every name cached inside 'directory'.  For a directory being deleted: its
        //      clusters are about to be freed, and may be reused by another directory.

        void RemoveWithChildren(FAT32ClusterIndex directory) noexcept
        {
            ReleaseIf([directory](const Slot &slot)
                      { return (slot.directory_.FirstClusterId() == directory) || (slot.parent_ == directory); });
        }

    private:
        static constexpr uint32_t NONE = 0xFFFFFFFF;

        struct Slot
        {
            uint64_t hash_ = 0;
            FAT32ClusterIndex parent_ = FAT32ClusterIndex(0);
            FAT32DirectoryCacheEntry directory_;
            uint32_t newer_ = NONE;          //  LRU list, towards the most recently used
            uint32_t older_ = NONE;          //  LRU list, towards the least recently used - and the free list
            uint32_t next_in_bucket_ = NONE;
            uint16_t name_length_ = 0;
            char name_[MAX_FILENAME_LENGTH]; //  name_length_ characters, not null-terminated
        };

        const uint64_t seed_;

        void *memory_ = nullptr;
        Slot *slots_ = nullptr;
        uint32_t *buckets_ = nullptr;

        size_t capacity_ = 0;
        size_t bucket_mask_ = 0;
        size_t size_ = 0;

        uint32_t most_recent_ = NONE;
        uint32_t least_recent_ = NONE;
        uint32_t free_ = NONE;

        uint64_t hits_ = 0;
        uint64_t misses_ = 0;

        static constexpr size_t BucketCountFor(size_t capacity) noexcept
        {
            //  A power of two, at least twice the capacity, so chains stay short.

            size_t buckets = 1;

            while (buckets < (capacity * 2))
            {
                buckets <<= 1;
            }

            return buckets;
        }

        uint64_t Hash(FAT32ClusterIndex parent, const char *name, size_t length) const noexcept
        {
            //  FNV-1a over the name with ASCII letters folded to upper case, so names that compare equal
            //      hash equal, started from the seed and the parent.  A matching hash is only a hint:
            //      Locate() compares the parent and the name themselves.

            uint64_t hash = seed_ ^ ((uint64_t)(uint32_t)parent * 0x9E3779B97F4A7C15ull);

            for (size_t i = 0; i < length; i++)
            {
                char c = name[i];

                if ((c >= 'a') && (c <= 'z'))
                {
                    c = c - ('a' - 'A');
                }

                hash ^= (uint8_t)c;
                hash *= 0x100000001B3ull;
            }

            return hash;
        }

        uint32_t Locate(FAT32ClusterIndex parent, const char *name, size_t length) const noexcept
        {
            if ((capacity_ == 0) || (length == 0) || (length > MAX_FILENAME_LENGTH))
            {
                return NONE;
            }

            const uint64_t hash = Hash(parent, name, length);

            for (uint32_t slot = buckets_[hash & bucket_mask_]; slot != NONE; slot = slots_[slot].next_in_bucket_)
            {
                const Slot &candidate = slots_[slot];

                //  The same comparison as the disk lookup's NameMatches(): equal length, then case-insensitive.

                if ((candidate.hash_ == hash) &&
                    (candidate.parent_ == parent) &&
                    (candidate.name_length_ == length) &&
                    (strnicmp(candidate.name_, name, length) == 0))
                {
                    return slot;
                }
            }

            return NONE;
        }

        void LinkAsMostRecent(uint32_t slot) noexcept
        {
            slots_[slot].newer_ = NONE;
            slots_[slot].older_ = most_recent_;

            if (most_recent_ != NONE)
            {
                slots_[most_recent_].newer_ = slot;
            }

            most_recent_ = slot;

            if (least_recent_ == NONE)
            {
                least_recent_ = slot;
            }
        }

        void Unlink(uint32_t slot) noexcept
        {
            const Slot &entry = slots_[slot];

            if (entry.newer_ != NONE)
            {
                slots_[entry.newer_].older_ = entry.older_;
            }
            else
            {
                most_recent_ = entry.older_;
            }

            if (entry.older_ != NONE)
            {
                slots_[entry.older_].newer_ = entry.newer_;
            }
            else
            {
                least_recent_ = entry.newer_;
            }
        }

        void MakeMostRecent(uint32_t slot) noexcept
        {
            if (slot != most_recent_)
            {
                Unlink(slot);
                LinkAsMostRecent(slot);
            }
        }

        //  Takes a slot out of its hash chain and the LRU list, and puts it on the free list.

        void Release(uint32_t slot) noexcept
        {
            uint32_t *link = &buckets_[slots_[slot].hash_ & bucket_mask_];

            while (*link != slot)
            {
                link = &slots_[*link].next_in_bucket_;
            }

            *link = slots_[slot].next_in_bucket_;

            Unlink(slot);

            slots_[slot].older_ = free_;
            free_ = slot;

            size_--;
        }

        template <typename Predicate>
        void ReleaseIf(Predicate matches) noexcept
        {
            uint32_t slot = most_recent_;

            while (slot != NONE)
            {
                const uint32_t older = slots_[slot].older_;

                if (matches(slots_[slot]))
                {
                    Release(slot);
                }

                slot = older;
            }
        }
    };
} // namespace filesystems::fat32
