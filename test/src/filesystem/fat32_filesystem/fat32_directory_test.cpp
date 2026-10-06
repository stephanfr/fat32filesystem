// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "../../cpputest_support.h"

#include "../../utility/mounted_test_filesystem.h"
#include "../../utility/test_task_owner.h"
#include "../../utility/stack_usage.h"

#include "filesystem/fat32_directory.h"
#include "filesystem/fat32_filesystem.h"
#include "filesystem/file_map.h"

namespace
{
    using namespace filesystems;
    using namespace filesystems::fat32;

    ut_utility::MountedTestFilesystem test_fs;

    //  How much stack one directory operation may use.  Target task stacks are 32 KB, and the caller -
    //      and any re-entry from a VisitDirectory callback - needs room too.

    constexpr size_t DIRECTORY_OPERATION_STACK_BUDGET = 10 * 1024;
    constexpr size_t REENTRANT_DIRECTORY_OPERATION_STACK_BUDGET = 12 * 1024;

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
        //      (one slot, plus one for the new end-of-directory marker) lands at cluster 12 idx 2.

        CHECK(root->CreateDirectory(minstd::fixed_string<>("OLDDIR")).Successful());

        auto first = root->GetDirectory(minstd::fixed_string<>("OLDDIR"));
        auto stale = root->GetDirectory(minstd::fixed_string<>("OLDDIR"));

        CHECK(first.Successful());
        CHECK(stale.Successful());

        const FAT32ClusterIndex olddir_cluster = static_cast<FAT32Directory *>(stale.Value().get())->FirstCluster();

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*first)->RemoveDirectory());

        //  FindNextEmptyCluster starts AT the last cluster it handed out, so OLDDIR's just-freed
        //      cluster would go straight to NEWDIR.  Then the stale handle's entry address AND
        //      first cluster both match NEWDIR, and no on-disk check can tell them apart.
        //      Occupy it, as if something else had been allocated in between - the case the
        //      identity check exists for.

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS,
                    test_fs.Adapter().UpdateFATTableEntry(olddir_cluster, FAT32EntryAllocatedAndEndOfFile));

        //  idx 2 is now 0xE5 with free space behind it, so the next 8.3 directory reuses
        //      idx 2 - the address the stale handle still holds - with a different cluster.

        auto newdir = root->CreateDirectory(minstd::fixed_string<>("NEWDIR"));
        CHECK(newdir.Successful());

        const FAT32ClusterIndex newdir_cluster = static_cast<FAT32Directory *>(newdir.Value().get())->FirstCluster();

        //  Premise: the slot really was reused, by a directory with a different first cluster.

        minstd::heap_buffer<uint8_t> raw_cluster(__os_dynamic_heap_resource, test_fs.Adapter().BytesPerCluster());

        CHECK(test_fs.ReadRawCluster(*second_cluster, raw_cluster.data()));
        CHECK(memcmp(raw_cluster.data() + (2 * 32), "NEWDIR     ", 11) == 0);
        CHECK((uint32_t)newdir_cluster != (uint32_t)olddir_cluster);

        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_HANDLE_IS_STALE, (*stale)->RemoveDirectory());

        CHECK(root->GetDirectory(minstd::fixed_string<>("NEWDIR")).Successful());
    }

    TEST(FAT32DirectoryTest, DeleteFileClearsLongFilenameEntriesAcrossAClusterBoundary)
    {
        auto root = test_fs.RootDirectory();

        const FAT32ClusterIndex root_cluster = test_fs.Adapter().RootDirectoryCluster();

        auto second_cluster = test_fs.Adapter().NextClusterInChain(root_cluster);      //  cluster 12

        CHECK(second_cluster.Successful());

        //  Cluster 12 has idx 0-1 in use.  A 136-character name needs 11 LFN entries plus the
        //      short entry: 12 slots, plus 1 for the end-of-directory marker because the run
        //      reaches it = 13.  idx 2-15 has 14, so the name fills idx 2-13 and leaves exactly idx 14-15 free.

        char filler[137];

        memset(filler, 'f', 132);
        memcpy(filler + 132, ".dat", 5);

        auto filler_file = root->OpenFile(minstd::fixed_string<MAX_FILENAME_LENGTH>(filler),
                                          static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));

        CHECK(filler_file.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*filler_file)->Close());

        const uint32_t lfn_after_filler = CountLiveLFNEntries(*second_cluster);

        //  A 25-character name needs 2 LFN entries plus the short entry: 3 slots, plus 1 for
        //      the end-of-directory marker = 4.  Only 2 remain, so the directory grows, and the
        //      run idx 14-15 + new idx 0-1 qualifies.  The LFN entries go in cluster 12 idx
        //      14-15 and the short entry goes in idx 0 of the new third cluster.

        minstd::fixed_string<MAX_FILENAME_LENGTH> straddling_name("straddling_entry_name.dat");

        auto straddling_file = root->OpenFile(straddling_name, static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));

        CHECK(straddling_file.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*straddling_file)->Close());

        //  Premise: the entry really does straddle.  If either check fails, the layout
        //      assumption is wrong - adjust the filler length, not the fix.

        auto third_cluster = test_fs.Adapter().NextClusterInChain(*second_cluster);

        CHECK(third_cluster.Successful());
        CHECK((uint32_t)*third_cluster < (uint32_t)FAT32EntryEOFThreshold);
        CHECK_EQUAL(lfn_after_filler + 2, CountLiveLFNEntries(*second_cluster));

        //  Deleting must clear the two LFN entries left behind in cluster 12.

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, root->DeleteFile(straddling_name));

        CHECK_EQUAL(lfn_after_filler, CountLiveLFNEntries(*second_cluster));
    }

    TEST(FAT32DirectoryTest, RemoveDirectoryClearsLongFilenameEntriesAcrossAClusterBoundary)
    {
        auto root = test_fs.RootDirectory();

        const FAT32ClusterIndex root_cluster = test_fs.Adapter().RootDirectoryCluster();

        //  "...Name.With.Leading.Periods.lNg" is empty.  Its three LFN entries sit at root
        //      cluster 2 idx 14-15 and cluster 12 idx 0; its short entry is cluster 12 idx 1.
        //      Removing it must clear the two LFN entries in cluster 2.

        const uint32_t lfn_in_first_cluster_before = CountLiveLFNEntries(root_cluster);

        auto directory = root->GetDirectory(minstd::fixed_string<MAX_FILENAME_LENGTH>("...Name.With.Leading.Periods.lNg"));

        CHECK(directory.Successful());

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*directory)->RemoveDirectory());

        CHECK_EQUAL(lfn_in_first_cluster_before - 2, CountLiveLFNEntries(root_cluster));
    }

    TEST(FAT32DirectoryTest, DeleteFilePropagatesRemoveEntryFailure)
    {
        auto root = test_fs.RootDirectory();

        auto file = root->OpenFile(minstd::fixed_string<>("failme.dat"),
                                   static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));

        CHECK(file.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());

        //  The first device write DeleteFile makes is RemoveEntry marking the entry deleted.

        test_fs.Device().SimulateWriteError(0);

        //  Pre-fix RemoveEntry's failure was discarded and this returned SUCCESS.

        CHECK(root->DeleteFile(minstd::fixed_string<>("failme.dat")) != FilesystemResultCodes::SUCCESS);
    }

    TEST(FAT32DirectoryTest, DeleteFileOfNeverWrittenFileSucceeds)
    {
        auto root = test_fs.RootDirectory();

        //  Created but never written: the entry's first cluster is 0 and there is no chain to
        //      release.  Must stay SUCCESS once ReleaseChain's result is checked.

        auto file = root->OpenFile(minstd::fixed_string<>("empty.dat"),
                                   static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));

        CHECK(file.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, root->DeleteFile(minstd::fixed_string<>("empty.dat")));
        CHECK_EQUAL(FilesystemResultCodes::FILE_NOT_FOUND, root->DeleteFile(minstd::fixed_string<>("empty.dat")));
    }

    TEST(FAT32DirectoryTest, RootLevelFilePathsHaveSingleSeparator)
    {
        auto root = test_fs.RootDirectory();

        auto file = root->OpenFile(minstd::fixed_string<>("rootfile.txt"),
                                   static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));

        CHECK(file.Successful());

        auto path = (*file)->AbsolutePath();

        CHECK(path.Successful());

        //  Pre-fix this is "//rootfile.txt", so the file map key never matches a
        //      caller-supplied "/rootfile.txt".

        STRCMP_EQUAL("/rootfile.txt", path->c_str());

        //  And the open file must be protected from deletion.

        CHECK_EQUAL(FilesystemResultCodes::FILE_ALREADY_OPENED_EXCLUSIVELY,
                    root->DeleteFile(minstd::fixed_string<>("rootfile.txt")));

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, root->DeleteFile(minstd::fixed_string<>("rootfile.txt")));
    }

    TEST(FAT32DirectoryTest, LongNameWithWrongChecksumIsIgnored)
    {
        //  The LFN run of "...Name.With.Leading.Periods.lNg" is root cluster 2 idx 14-15 and
        //      cluster 12 idx 0.  Byte 13 of an LFN entry is the short-name checksum.  Corrupt it
        //      in all three: the run is now orphaned and must not name anything.

        const FAT32ClusterIndex root_cluster = test_fs.Adapter().RootDirectoryCluster();

        auto second_cluster = test_fs.Adapter().NextClusterInChain(root_cluster);

        CHECK(second_cluster.Successful());

        minstd::heap_buffer<uint8_t> buffer(__os_dynamic_heap_resource, test_fs.Adapter().BytesPerCluster());

        CHECK(test_fs.ReadRawCluster(root_cluster, buffer.data()));
        buffer.data()[(14 * 32) + 13] ^= 0xFF;
        buffer.data()[(15 * 32) + 13] ^= 0xFF;
        CHECK(test_fs.Adapter().WriteCluster(root_cluster, buffer.data()) == BlockIOResultCodes::SUCCESS);

        CHECK(test_fs.ReadRawCluster(*second_cluster, buffer.data()));
        buffer.data()[(0 * 32) + 13] ^= 0xFF;
        CHECK(test_fs.Adapter().WriteCluster(*second_cluster, buffer.data()) == BlockIOResultCodes::SUCCESS);

        auto root = test_fs.RootDirectory();

        CHECK_FALSE(root->GetDirectory(minstd::fixed_string<MAX_FILENAME_LENGTH>("...Name.With.Leading.Periods.lNg")).Successful());
    }

    TEST(FAT32DirectoryTest, RenameOpenFileIsRejected)
    {
        auto root = test_fs.RootDirectory();

        auto subdir = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir.Successful());

        minstd::fixed_string<MAX_FILENAME_LENGTH> name("Lorem ipsum dolor sit amet.text");

        auto file = (*subdir)->OpenFile(name, FileModes::READ);

        CHECK(file.Successful());

        CHECK_EQUAL(FilesystemResultCodes::FILE_ALREADY_OPENED_EXCLUSIVELY,
                    (*subdir)->RenameFile(name, minstd::fixed_string<>("RENAMED.TXT")));

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());
    }

    TEST(FAT32DirectoryTest, FailedRenameDoesNotLeaveTwoEntries)
    {
        auto root = test_fs.RootDirectory();

        auto subdir = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir.Successful());

        minstd::fixed_string<MAX_FILENAME_LENGTH> name("Lorem ipsum dolor sit amet.text");

        //  Rename makes two writes: creating "RENAMED.TXT" (one cluster), then marking the old
        //      entry deleted (cluster 3).  Let the first through and fail the second.

        test_fs.Device().SimulateWriteError(1);

        CHECK((*subdir)->RenameFile(name, minstd::fixed_string<>("RENAMED.TXT")) != FilesystemResultCodes::SUCCESS);

        //  Exactly one entry may reference the file's chain: the original.

        CHECK_FALSE((*subdir)->OpenFile(minstd::fixed_string<>("RENAMED.TXT"), FileModes::READ).Successful());

        auto original = (*subdir)->OpenFile(name, FileModes::READ);

        CHECK(original.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*original)->Close());
    }

    TEST(FAT32DirectoryTest, DeletedSingleEntrySlotIsReused)
    {
        auto root = test_fs.RootDirectory();

        //  SUBDIR3 is an empty 8.3 directory at root cluster 2 idx 3, between two live entries.
        //      Removing it leaves a one-slot hole - exactly what an 8.3 name needs.

        auto subdir3 = root->GetDirectory(minstd::fixed_string<>("SUBDIR3"));

        CHECK(subdir3.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*subdir3)->RemoveDirectory());

        CHECK(root->CreateDirectory(minstd::fixed_string<>("NEWDIR")).Successful());

        minstd::heap_buffer<uint8_t> buffer(__os_dynamic_heap_resource, test_fs.Adapter().BytesPerCluster());

        CHECK(test_fs.ReadRawCluster(test_fs.Adapter().RootDirectoryCluster(), buffer.data()));
        CHECK(memcmp(buffer.data() + (3 * 32), "NEWDIR     ", 11) == 0);
    }

    TEST(FAT32DirectoryTest, FilesAndDirectoriesShareOneNamespace)
    {
        auto root = test_fs.RootDirectory();

        auto file = root->OpenFile(minstd::fixed_string<>("SUBDIR1"), static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));

        CHECK_EQUAL(FilesystemResultCodes::FILENAME_ALREADY_IN_USE, file.ResultCode());
    }

    TEST(FAT32DirectoryTest, FileCanBeOpenedByItsShortAlias)
    {
        auto root = test_fs.RootDirectory();
        auto subdir = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir.Successful());

        auto file = (*subdir)->OpenFile(minstd::fixed_string<>("LOREMI~1.TEX"), FileModes::READ);

        CHECK(file.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());
    }

    TEST(FAT32DirectoryTest, ShortAliasesStayUnique)
    {
        auto root = test_fs.RootDirectory();
        auto subdir3 = root->GetDirectory(minstd::fixed_string<>("SUBDIR3"));     //  empty, cluster 5

        CHECK(subdir3.Successful());

        auto first = (*subdir3)->OpenFile(minstd::fixed_string<>("README~1.TXT"), static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));
        CHECK(first.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*first)->Close());

        auto second = (*subdir3)->OpenFile(minstd::fixed_string<>("readme.txt"), static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));
        CHECK(second.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*second)->Close());

        //  Count live short entries named README~1.TXT in SUBDIR3's cluster.

        minstd::heap_buffer<uint8_t> buffer(__os_dynamic_heap_resource, test_fs.Adapter().BytesPerCluster());

        CHECK(test_fs.ReadRawCluster(FAT32ClusterIndex(5), buffer.data()));

        uint32_t matches = 0;

        for (uint32_t i = 0; i < test_fs.Adapter().BytesPerCluster() / 32; i++)
        {
            const uint8_t *entry = buffer.data() + (i * 32);

            if ((entry[11] != 0x0F) && (entry[0] != 0xE5) && (entry[0] != 0x00) && (memcmp(entry, "README~1TXT", 11) == 0))
            {
                matches++;
            }
        }

        CHECK_EQUAL(1, matches);
    }

    TEST(FAT32DirectoryTest, OpenFileIsProtectedUnderAnySpelling)
    {
        auto root = test_fs.RootDirectory();
        auto subdir = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir.Successful());

        auto reader = (*subdir)->OpenFile(minstd::fixed_string<MAX_FILENAME_LENGTH>("Lorem ipsum dolor sit amet.text"), FileModes::READ);

        CHECK(reader.Successful());

        CHECK_EQUAL(FilesystemResultCodes::FILE_ALREADY_OPENED_EXCLUSIVELY,
                    (*subdir)->DeleteFile(minstd::fixed_string<MAX_FILENAME_LENGTH>("LOREM IPSUM DOLOR SIT AMET.TEXT")));
        CHECK_EQUAL(FilesystemResultCodes::FILE_ALREADY_OPENED_EXCLUSIVELY,
                    (*subdir)->DeleteFile(minstd::fixed_string<>("LOREMI~1.TEX")));
        CHECK_FALSE((*subdir)->OpenFile(minstd::fixed_string<MAX_FILENAME_LENGTH>("lorem ipsum dolor sit amet.text"), FileModes::WRITE).Successful());

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*reader)->Close());
    }

    TEST(FAT32DirectoryTest, CreatedFileReportsItsOnDiskEntry)
    {
        auto root = test_fs.RootDirectory();

        auto file = root->OpenFile(minstd::fixed_string<>("rootfile.txt"),
                                   static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));

        CHECK(file.Successful());

        //  The new file's entry must be the one written to disk: long name, size 0.

        auto name = (*file)->Filename();

        CHECK(name.Successful());
        STRCMP_EQUAL("rootfile.txt", name->c_str());

        auto entry = (*file)->DirectoryEntry();

        CHECK(entry.Successful());
        CHECK_EQUAL(0U, entry->Size());

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());
    }

    TEST(FAT32DirectoryTest, RemovingAParentReachedThroughDotDotDeletesItsRealEntry)
    {
        auto root = test_fs.RootDirectory();

        auto parent = root->CreateDirectory(minstd::fixed_string<>("PARENT"));
        CHECK(parent.Successful());

        auto child = (*parent)->CreateDirectory(minstd::fixed_string<>("CHILD"));
        CHECK(child.Successful());

        auto parent_via_dot_dot = (*child)->GetDirectory(minstd::fixed_string<>(".."));
        CHECK(parent_via_dot_dot.Successful());

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*child)->RemoveDirectory());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*parent_via_dot_dot)->RemoveDirectory());

        //  PARENT's entry in the root must be gone, not left pointing at freed clusters.

        CHECK_FALSE(root->GetDirectory(minstd::fixed_string<>("PARENT")).Successful());
    }

    TEST(FAT32DirectoryTest, SystemAttributeFilesAreOrdinaryFiles)
    {
        //  Give Lorem (SUBDIR1 cluster 3 idx 14) ARCHIVE|SYSTEM.

        minstd::heap_buffer<uint8_t> buffer(__os_dynamic_heap_resource, test_fs.Adapter().BytesPerCluster());

        CHECK(test_fs.ReadRawCluster(FAT32ClusterIndex(3), buffer.data()));
        buffer.data()[(14 * 32) + 11] = 0x24;
        CHECK(test_fs.Adapter().WriteCluster(FAT32ClusterIndex(3), buffer.data()) == BlockIOResultCodes::SUCCESS);

        auto root = test_fs.RootDirectory();
        auto subdir = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir.Successful());

        auto file = (*subdir)->OpenFile(minstd::fixed_string<MAX_FILENAME_LENGTH>("Lorem ipsum dolor sit amet.text"), FileModes::READ);

        CHECK(file.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());
    }

    TEST(FAT32DirectoryTest, EmptyRootDirectoryIsEmptyNotAnError)
    {
        //  A root with no live entries: empty_fat32.img with its volume label (root idx 0) deleted.

        test_fs.Unmount();
        CHECK(test_fs.Mount("./test/data/empty_fat32.img"));

        const FAT32ClusterIndex root_cluster = test_fs.Adapter().RootDirectoryCluster();

        minstd::heap_buffer<uint8_t> buffer(__os_dynamic_heap_resource, test_fs.Adapter().BytesPerCluster());

        CHECK(test_fs.ReadRawCluster(root_cluster, buffer.data()));
        buffer.data()[0] = 0xE5;
        CHECK(test_fs.Adapter().WriteCluster(root_cluster, buffer.data()) == BlockIOResultCodes::SUCCESS);

        auto root = test_fs.RootDirectory();

        //  Lookups find nothing - they do not fail.

        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_NOT_FOUND, root->GetDirectory(minstd::fixed_string<>("NOSUCH")).ResultCode());

        //  And the directory can be written to.

        auto file = root->OpenFile(minstd::fixed_string<>("NEW.TXT"), static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));

        CHECK(file.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());
    }

    TEST(FAT32DirectoryTest, LongNameEntryWithReservedAttributeBitsIsStillALongNameEntry)
    {
        //  Set reserved attribute bit 6 on Lorem's LFN ordinal 1 (SUBDIR1 cluster 3 idx 13).

        minstd::heap_buffer<uint8_t> buffer(__os_dynamic_heap_resource, test_fs.Adapter().BytesPerCluster());

        CHECK(test_fs.ReadRawCluster(FAT32ClusterIndex(3), buffer.data()));
        buffer.data()[(13 * 32) + 11] = 0x4F;
        CHECK(test_fs.Adapter().WriteCluster(FAT32ClusterIndex(3), buffer.data()) == BlockIOResultCodes::SUCCESS);

        auto root = test_fs.RootDirectory();
        auto subdir = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir.Successful());

        auto file = (*subdir)->OpenFile(minstd::fixed_string<MAX_FILENAME_LENGTH>("Lorem ipsum dolor sit amet.text"), FileModes::READ);

        CHECK(file.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());
    }

    TEST(FAT32DirectoryTest, NewFilesAreMarkedArchive)
    {
        auto root = test_fs.RootDirectory();

        auto file = root->OpenFile(minstd::fixed_string<>("ARCHIVE.TXT"), static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));

        CHECK(file.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());

        //  Find the short entry in the root (chain 2 -> 12) and check its attribute byte.

        minstd::heap_buffer<uint8_t> buffer(__os_dynamic_heap_resource, test_fs.Adapter().BytesPerCluster());

        const uint32_t root_clusters[] = {2, 12};

        uint32_t found = 0;

        for (uint32_t cluster : root_clusters)
        {
            CHECK(test_fs.ReadRawCluster(FAT32ClusterIndex(cluster), buffer.data()));

            for (uint32_t i = 0; i < test_fs.Adapter().BytesPerCluster() / 32; i++)
            {
                const uint8_t *entry = buffer.data() + (i * 32);

                if (memcmp(entry, "ARCHIVE TXT", 11) == 0)
                {
                    CHECK_EQUAL(0x20, entry[11]);
                    found++;
                }
            }
        }

        CHECK_EQUAL(1U, found);
    }

    TEST(FAT32DirectoryTest, DirectoryWithACyclicChainFailsInsteadOfHanging)
    {
        //  SUBDIR3 (cluster 5) holds only '.' and '..'.  Mark every other slot deleted, so there is
        //      no end-of-directory marker, and point the cluster at itself.

        const uint32_t bytes_per_cluster = test_fs.Adapter().BytesPerCluster();

        minstd::heap_buffer<uint8_t> buffer(__os_dynamic_heap_resource, bytes_per_cluster);

        CHECK(test_fs.ReadRawCluster(FAT32ClusterIndex(5), buffer.data()));

        for (uint32_t i = 2; i < bytes_per_cluster / 32; i++)
        {
            buffer.data()[i * 32] = 0xE5;
        }

        CHECK(test_fs.Adapter().WriteCluster(FAT32ClusterIndex(5), buffer.data()) == BlockIOResultCodes::SUCCESS);
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, test_fs.Adapter().UpdateFATTableEntry(FAT32ClusterIndex(5), FAT32ClusterIndex(5)));

        auto root = test_fs.RootDirectory();
        auto subdir3 = root->GetDirectory(minstd::fixed_string<>("SUBDIR3"));

        CHECK(subdir3.Successful());

        auto file = (*subdir3)->OpenFile(minstd::fixed_string<>("NOFILE.TXT"), FileModes::READ);

        CHECK_EQUAL(FilesystemResultCodes::FAT32_CLUSTER_CHAIN_IS_CORRUPT, file.ResultCode());
    }

    TEST(FAT32DirectoryTest, DirectoryCannotGrowPastTheSpecificationLimit)
    {
        FAT32BlockIOAdapter &adapter = test_fs.Adapter();

        const uint32_t bytes_per_cluster = adapter.BytesPerCluster();
        const uint32_t entries_per_cluster = bytes_per_cluster / 32;
        const uint32_t max_clusters = 65536 / entries_per_cluster;          //  4096 with 512-byte clusters

        //  Make SUBDIR3 (cluster 5) exactly 65,536 entries with no free slot.  Fill cluster 5 after
        //      '.' and '..', then chain on max_clusters - 1 full clusters from the free run at 1000.

        minstd::heap_buffer<uint8_t> buffer(__os_dynamic_heap_resource, bytes_per_cluster);

        CHECK(test_fs.ReadRawCluster(FAT32ClusterIndex(5), buffer.data()));

        for (uint32_t i = 2; i < entries_per_cluster; i++)
        {
            uint8_t *entry = buffer.data() + (i * 32);

            memset(entry, 0, 32);
            memcpy(entry, "FILLER  BIN", 11);
            entry[11] = 0x20;
        }

        CHECK(adapter.WriteCluster(FAT32ClusterIndex(5), buffer.data()) == BlockIOResultCodes::SUCCESS);

        //  The chained clusters are full of the same filler entry - overwrite '.' and '..'.

        memcpy(buffer.data(), buffer.data() + (2 * 32), 32);
        memcpy(buffer.data() + 32, buffer.data() + (2 * 32), 32);

        uint32_t previous = 5;

        for (uint32_t cluster = 1000; cluster < 1000 + max_clusters - 1; cluster++)
        {
            CHECK(adapter.WriteCluster(FAT32ClusterIndex(cluster), buffer.data()) == BlockIOResultCodes::SUCCESS);
            CHECK_EQUAL(FilesystemResultCodes::SUCCESS, adapter.UpdateFATTableEntry(FAT32ClusterIndex(previous), FAT32ClusterIndex(cluster)));

            previous = cluster;
        }

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, adapter.UpdateFATTableEntry(FAT32ClusterIndex(previous), FAT32EntryAllocatedAndEndOfFile));

        //  The directory is full and at the limit, so it cannot grow to take one more entry.

        auto root = test_fs.RootDirectory();
        auto subdir3 = root->GetDirectory(minstd::fixed_string<>("SUBDIR3"));

        CHECK(subdir3.Successful());

        auto file = (*subdir3)->OpenFile(minstd::fixed_string<>("NEWFILE.TXT"), static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));

        CHECK_EQUAL(FilesystemResultCodes::FAT32_UNABLE_TO_FIND_EMPTY_BLOCK_OF_DIRECTORY_ENTRIES, file.ResultCode());
    }

    TEST(FAT32DirectoryTest, VisitDirectoryHoldsTheFilesystemLockAndAllowsReentry)
    {
        auto root = test_fs.RootDirectory();

        uint32_t visited = 0;
        bool lock_free_in_callback = true;
        bool reentry_succeeded = false;

        FilesystemResultCodes result = root->VisitDirectory([&](const FilesystemDirectoryEntry &) -> FilesystemDirectoryVisitorCallbackStatus
                                                            {
                                                                visited++;

                                                                //  Another task must be shut out while the walk is in progress...

                                                                lock_free_in_callback = LockIsFree(test_fs.Filesystem().FilesystemLock());

                                                                //  ...but this task may re-enter the filesystem: the lock is recursive.

                                                                reentry_succeeded = root->GetDirectory(minstd::fixed_string<>("SUBDIR1")).Successful();

                                                                return FilesystemDirectoryVisitorCallbackStatus::FINISHED;
                                                            });

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, result);
        CHECK_EQUAL(1U, visited);
#if FILESYSTEM_LOCKING
        CHECK_FALSE(lock_free_in_callback);     //  compiled out, there is no lock to hold
#endif
        CHECK(reentry_succeeded);
        CHECK(LockIsFree(test_fs.Filesystem().FilesystemLock()));
    }

    TEST(FAT32DirectoryTest, EveryOperationReleasesTheFilesystemLock)
    {
        FilesystemMutex &lock = test_fs.Filesystem().FilesystemLock();

        auto root = test_fs.RootDirectory();

        auto subdir3 = root->GetDirectory(minstd::fixed_string<>("SUBDIR3"));
        CHECK(subdir3.Successful());
        CHECK(LockIsFree(lock));

        auto file = (*subdir3)->OpenFile(minstd::fixed_string<>("LOCKS.TXT"), static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));
        CHECK(file.Successful());
        CHECK(LockIsFree(lock));

        minstd::heap_buffer<uint8_t> payload(__os_dynamic_heap_resource, 600);      //  crosses a cluster boundary

        for (uint32_t i = 0; i < 600; i++)
        {
            uint8_t byte = 0x5A;

            payload.append(&byte, 1);
        }

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Write(payload));
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Append(payload));     //  nests: Append -> SeekEnd -> Seek, Write -> SeekEnd
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Seek(0));
        CHECK(LockIsFree(lock));

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());
        CHECK(LockIsFree(lock));

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*subdir3)->RenameFile(minstd::fixed_string<>("LOCKS.TXT"), minstd::fixed_string<>("LOCKS2.TXT")));
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*subdir3)->DeleteFile(minstd::fixed_string<>("LOCKS2.TXT")));
        CHECK(LockIsFree(lock));

        auto child = (*subdir3)->CreateDirectory(minstd::fixed_string<>("CHILD"));
        CHECK(child.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*child)->RemoveDirectory());
        CHECK(LockIsFree(lock));

        CHECK(test_fs.Filesystem().GetDirectory(minstd::fixed_string<>("/SUBDIR1")).Successful());
        CHECK(LockIsFree(lock));
    }

    TEST(FAT32DirectoryTest, FailedOperationsReleaseTheFilesystemLock)
    {
        FilesystemMutex &lock = test_fs.Filesystem().FilesystemLock();

        auto root = test_fs.RootDirectory();

        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_NOT_FOUND, root->GetDirectory(minstd::fixed_string<>("NOSUCH")).ResultCode());
        CHECK(LockIsFree(lock));

        CHECK_FALSE(root->OpenFile(minstd::fixed_string<>("NOSUCH.TXT"), FileModes::READ).Successful());
        CHECK(LockIsFree(lock));

        CHECK_EQUAL(FilesystemResultCodes::FILE_NOT_FOUND, root->DeleteFile(minstd::fixed_string<>("NOSUCH.TXT")));
        CHECK(LockIsFree(lock));

        CHECK_FALSE(test_fs.Filesystem().GetDirectory(minstd::fixed_string<>("/NOSUCH")).Successful());
        CHECK(LockIsFree(lock));
    }

    TEST(FAT32DirectoryTest, OpenFileStaysProtectedAfterItsDirectoryIsRenamed)
    {
        auto root = test_fs.RootDirectory();
        auto subdir1 = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir1.Successful());

        minstd::fixed_string<MAX_FILENAME_LENGTH> name("Lorem ipsum dolor sit amet.text");

        auto writer = (*subdir1)->OpenFile(name, FileModes::WRITE);

        CHECK(writer.Successful());

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, root->RenameDirectory(minstd::fixed_string<>("SUBDIR1"), minstd::fixed_string<>("MOVED")));

        auto moved = root->GetDirectory(minstd::fixed_string<>("MOVED"));

        CHECK(moved.Successful());

        //  Same file, new path: still open, still exclusive.

        CHECK_EQUAL(FilesystemResultCodes::FILE_ALREADY_OPENED_EXCLUSIVELY, (*moved)->DeleteFile(name));
        CHECK_FALSE((*moved)->OpenFile(name, FileModes::WRITE).Successful());

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*writer)->Close());
    }

    TEST(FAT32DirectoryTest, DescendantCacheEntriesDoNotSurviveARename)
    {
        auto root = test_fs.RootDirectory();
        auto subdir1 = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir1.Successful());

        //  Cache "/SUBDIR1/this is a long subdirectory name".

        CHECK((*subdir1)->GetDirectory(minstd::fixed_string<MAX_FILENAME_LENGTH>("this is a long subdirectory name")).Successful());

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, root->RenameDirectory(minstd::fixed_string<>("SUBDIR1"), minstd::fixed_string<>("MOVED")));

        CHECK_FALSE(test_fs.Filesystem().GetDirectory(minstd::fixed_string<MAX_FILESYSTEM_PATH_LENGTH>("/SUBDIR1/this is a long subdirectory name")).Successful());
        CHECK(test_fs.Filesystem().GetDirectory(minstd::fixed_string<MAX_FILESYSTEM_PATH_LENGTH>("/MOVED/this is a long subdirectory name")).Successful());
    }

    TEST(FAT32DirectoryTest, HandleToARemovedDirectoryIsStale)
    {
        auto root = test_fs.RootDirectory();

        auto doomed = root->CreateDirectory(minstd::fixed_string<>("DOOMED"));
        CHECK(doomed.Successful());

        auto second_handle = root->GetDirectory(minstd::fixed_string<>("DOOMED"));
        CHECK(second_handle.Successful());

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*doomed)->RemoveDirectory());

        //  DOOMED's cluster is free.  Creating a file through the old handle would write a directory
        //      entry into it.

        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_HANDLE_IS_STALE,
                    (*second_handle)->OpenFile(minstd::fixed_string<>("X.TXT"), static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE)).ResultCode());
    }

    TEST(FAT32DirectoryTest, HandlesToARenamedDirectoryAndItsDescendantsAreStale)
    {
        auto root = test_fs.RootDirectory();
        auto subdir1 = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));
        CHECK(subdir1.Successful());

        auto child = (*subdir1)->GetDirectory(minstd::fixed_string<MAX_FILENAME_LENGTH>("this is a long subdirectory name"));
        CHECK(child.Successful());

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, root->RenameDirectory(minstd::fixed_string<>("SUBDIR1"), minstd::fixed_string<>("MOVED")));

        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_HANDLE_IS_STALE,
                    (*subdir1)->OpenFile(minstd::fixed_string<>("NEW.TXT"), static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE)).ResultCode());
        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_HANDLE_IS_STALE,
                    (*child)->GetDirectory(minstd::fixed_string<>(".")).ResultCode());

        //  The root and fresh handles are fine.

        auto moved = root->GetDirectory(minstd::fixed_string<>("MOVED"));
        CHECK(moved.Successful());

        auto file = (*moved)->OpenFile(minstd::fixed_string<>("NEW.TXT"), static_cast<FileModes>(FileModes::CREATE | FileModes::WRITE));
        CHECK(file.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*file)->Close());
    }

    TEST(FAT32DirectoryTest, DotEntriesAreNotNames)
    {
        auto root = test_fs.RootDirectory();
        auto subdir1 = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir1.Successful());

        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_NOT_FOUND,
                    (*subdir1)->RenameDirectory(minstd::fixed_string<>("."), minstd::fixed_string<>("LOOP")));
        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_NOT_FOUND,
                    (*subdir1)->RenameDirectory(minstd::fixed_string<>(".."), minstd::fixed_string<>("UP")));

        //  Nothing was created...

        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_NOT_FOUND, (*subdir1)->GetDirectory(minstd::fixed_string<>("LOOP")).ResultCode());
        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_NOT_FOUND, (*subdir1)->GetDirectory(minstd::fixed_string<>("UP")).ResultCode());

        //  ...and both dot entries are still in place.

        const FAT32ClusterIndex subdir1_cluster = static_cast<FAT32Directory &>(**subdir1).FirstCluster();

        minstd::heap_buffer<uint8_t> raw(__os_dynamic_heap_resource, test_fs.Adapter().BytesPerCluster());

        CHECK(test_fs.ReadRawCluster(subdir1_cluster, raw.data()));
        CHECK(memcmp(raw.data(), ".          ", 11) == 0);
        CHECK(memcmp(raw.data() + 32, "..         ", 11) == 0);

        //  '.' and '..' as directory names still work - they are handled before any lookup.

        CHECK((*subdir1)->GetDirectory(minstd::fixed_string<>("..")).Successful());
    }

    TEST(FAT32DirectoryTest, NameWithEmbeddedSpaceGetsALongNameAndAnAlias)
    {
        auto root = test_fs.RootDirectory();

        auto created = root->CreateDirectory(minstd::fixed_string<>("MY DIR"));

        CHECK(created.Successful());

        //  Found by the name it was given, and by its generated 8.3 alias.

        CHECK(root->GetDirectory(minstd::fixed_string<>("MY DIR")).Successful());
        CHECK(root->GetDirectory(minstd::fixed_string<>("MYDIR~1")).Successful());

        //  The space-less spelling is a different name, and is still free.

        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_NOT_FOUND, root->GetDirectory(minstd::fixed_string<>("MYDIR")).ResultCode());
        CHECK(root->CreateDirectory(minstd::fixed_string<>("MYDIR")).Successful());
    }

    TEST(FAT32DirectoryTest, PathsWithDotComponentsAreRejected)
    {
        CHECK_EQUAL(FilesystemResultCodes::ILLEGAL_PATH, test_fs.Filesystem().GetDirectory(minstd::fixed_string<>("/SUBDIR1/.")).ResultCode());
        CHECK_EQUAL(FilesystemResultCodes::ILLEGAL_PATH, test_fs.Filesystem().GetDirectory(minstd::fixed_string<>("/SUBDIR1/..")).ResultCode());
    }

    TEST(FAT32DirectoryTest, OutOfOrderLongNameRunFallsBackToTheShortName)
    {
        //  Lorem's long name is SUBDIR1 cluster 3 idx 11-13 (ordinals 0x43, 0x02, 0x01); its short
        //      entry is idx 14.  Give the last LFN entry a wrong ordinal.

        minstd::heap_buffer<uint8_t> raw(__os_dynamic_heap_resource, test_fs.Adapter().BytesPerCluster());

        CHECK(test_fs.ReadRawCluster(FAT32ClusterIndex(3), raw.data()));
        CHECK_EQUAL(0x43, raw.data()[11 * 32]);
        CHECK_EQUAL(0x01, raw.data()[13 * 32]);
        raw.data()[13 * 32] = 0x02;
        CHECK(test_fs.Adapter().WriteCluster(FAT32ClusterIndex(3), raw.data()) == BlockIOResultCodes::SUCCESS);

        auto root = test_fs.RootDirectory();
        auto subdir1 = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir1.Successful());

        //  The broken run no longer names the file; its 8.3 alias still does.

        CHECK_EQUAL(FilesystemResultCodes::FILE_NOT_FOUND,
                    (*subdir1)->OpenFile(minstd::fixed_string<MAX_FILENAME_LENGTH>("Lorem ipsum dolor sit amet.text"), FileModes::READ).ResultCode());

        auto by_alias = (*subdir1)->OpenFile(minstd::fixed_string<>("LOREMI~1.TEX"), FileModes::READ);

        CHECK(by_alias.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*by_alias)->Close());
    }

    TEST(FAT32DirectoryTest, RenamingADirectoryKeepsItsSubdirectoriesCached)
    {
        auto root = test_fs.RootDirectory();
        FAT32Filesystem &filesystem = test_fs.Filesystem();

        auto outer = root->CreateDirectory(minstd::fixed_string<>("OUTER"));

        CHECK(outer.Successful());
        CHECK((*outer)->CreateDirectory(minstd::fixed_string<>("INNER")).Successful());

        //  Resolve /OUTER/INNER once, so both steps are cached.

        CHECK(filesystem.GetDirectory(minstd::fixed_string<>("/OUTER/INNER")).Successful());

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, root->RenameDirectory(minstd::fixed_string<>("OUTER"), minstd::fixed_string<>("RENAMED")));

        //  Only OUTER's own name went stale.  INNER is cached under OUTER's first cluster, which the
        //      rename did not change, so the second step of this lookup is a cache hit.

        const uint64_t hits_before = filesystem.Statistics().DirectoryCacheHits();

        CHECK(filesystem.GetDirectory(minstd::fixed_string<>("/RENAMED/INNER")).Successful());

        CHECK_EQUAL(hits_before + 1, filesystem.Statistics().DirectoryCacheHits());

        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_NOT_FOUND, filesystem.GetDirectory(minstd::fixed_string<>("/OUTER/INNER")).ResultCode());
    }

    TEST(FAT32DirectoryTest, DirectoryLookupsIgnoreCase)
    {
        FAT32Filesystem &filesystem = test_fs.Filesystem();

        CHECK(filesystem.GetDirectory(minstd::fixed_string<>("/SUBDIR1")).Successful());

        const uint64_t hits_before = filesystem.Statistics().DirectoryCacheHits();

        //  FAT compares names without case, so this is the directory just cached.

        CHECK(filesystem.GetDirectory(minstd::fixed_string<>("/subdir1")).Successful());

        CHECK_EQUAL(hits_before + 1, filesystem.Statistics().DirectoryCacheHits());
    }

    TEST(FAT32DirectoryTest, DirectoryLookupsDoNotGrowTheHeap)
    {
        constexpr uint32_t DIRECTORY_COUNT = 20;

        auto root = test_fs.RootDirectory();
        FAT32Filesystem &filesystem = test_fs.Filesystem();

        char path[] = "/DIR00";

        for (uint32_t i = 0; i < DIRECTORY_COUNT; i++)
        {
            path[4] = '0' + (i / 10);
            path[5] = '0' + (i % 10);

            CHECK(root->CreateDirectory(minstd::fixed_string<>(path + 1)).Successful());
        }

        //  The cache took all of its memory when the volume was mounted (the tests map the cache heap
        //      onto the dynamic heap).  Filling it must not take any more.

        const size_t heap_bytes_before = __os_dynamic_heap_core.bytes_in_use();

        for (uint32_t i = 0; i < DIRECTORY_COUNT; i++)
        {
            path[4] = '0' + (i / 10);
            path[5] = '0' + (i % 10);

            CHECK(filesystem.GetDirectory(minstd::fixed_string<>(path)).Successful());
        }

        CHECK_EQUAL(heap_bytes_before, __os_dynamic_heap_core.bytes_in_use());
    }

    TEST(FAT32DirectoryTest, ARenamedDirectoryIsGoneUnderBothItsOldNames)
    {
        auto root = test_fs.RootDirectory();
        FAT32Filesystem &filesystem = test_fs.Filesystem();

        CHECK(root->CreateDirectory(minstd::fixed_string<>("Long Directory Name")).Successful());

        //  Cache it under its long name and under its 8.3 alias.

        CHECK(filesystem.GetDirectory(minstd::fixed_string<>("/Long Directory Name")).Successful());
        CHECK(filesystem.GetDirectory(minstd::fixed_string<>("/LONGDI~1")).Successful());

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, root->RenameDirectory(minstd::fixed_string<>("Long Directory Name"), minstd::fixed_string<>("Other Name")));

        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_NOT_FOUND, filesystem.GetDirectory(minstd::fixed_string<>("/Long Directory Name")).ResultCode());
        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_NOT_FOUND, filesystem.GetDirectory(minstd::fixed_string<>("/LONGDI~1")).ResultCode());
        CHECK(filesystem.GetDirectory(minstd::fixed_string<>("/Other Name")).Successful());
    }

    TEST(FAT32DirectoryTest, ARemovedDirectoryIsNotFoundThroughTheCache)
    {
        auto root = test_fs.RootDirectory();
        FAT32Filesystem &filesystem = test_fs.Filesystem();

        CHECK(root->CreateDirectory(minstd::fixed_string<>("DOOMED")).Successful());

        auto doomed = filesystem.GetDirectory(minstd::fixed_string<>("/DOOMED"));

        CHECK(doomed.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*doomed)->RemoveDirectory());

        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_NOT_FOUND, filesystem.GetDirectory(minstd::fixed_string<>("/DOOMED")).ResultCode());
    }

    TEST(FAT32DirectoryTest, DirectoryOperationsStayWithinTheStackBudget)
    {
        auto root = test_fs.RootDirectory();
        auto subdir1 = root->GetDirectory(minstd::fixed_string<>("SUBDIR1"));

        CHECK(subdir1.Successful());

        auto child = (*subdir1)->GetDirectory(minstd::fixed_string<MAX_FILENAME_LENGTH>("this is a long subdirectory name"));

        CHECK(child.Successful());

        FilesystemDirectory &directory = **child;
        const minstd::fixed_string<> dot_dot("..");

        //  The deepest path: after any directory is removed, GetDirectory("..") re-validates this
        //      handle by resolving its own path, then resolves its parent's.  Removing a scratch
        //      directory makes every handle re-validate.

        auto scratch = root->CreateDirectory(minstd::fixed_string<>("SCRATCH1"));

        CHECK(scratch.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*scratch)->RemoveDirectory());

        ut_utility::PaintStack();
        const bool found = directory.GetDirectory(dot_dot).Successful();
        const size_t used = ut_utility::StackBytesUsedSincePaint();

        CHECK(found);

        //  The same call made from inside a VisitDirectory callback, where the walk's own frames sit
        //      underneath it.

        auto scratch2 = root->CreateDirectory(minstd::fixed_string<>("SCRATCH2"));

        CHECK(scratch2.Successful());
        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, (*scratch2)->RemoveDirectory());

        bool found_in_callback = false;

        FilesystemDirectoryVisitorCallback callback = [&](const FilesystemDirectoryEntry &) -> FilesystemDirectoryVisitorCallbackStatus
        {
            found_in_callback = directory.GetDirectory(dot_dot).Successful();

            return FilesystemDirectoryVisitorCallbackStatus::FINISHED;
        };

        ut_utility::PaintStack();
        const FilesystemResultCodes visit_result = root->VisitDirectory(callback);
        const size_t used_in_callback = ut_utility::StackBytesUsedSincePaint();

        CHECK_EQUAL(FilesystemResultCodes::SUCCESS, visit_result);
        CHECK(found_in_callback);

#ifndef __SANITIZE_ADDRESS__
        CHECK_TEXT(used < DIRECTORY_OPERATION_STACK_BUDGET,
                   StringFromFormat("GetDirectory(\"..\") used %lu bytes of stack", (unsigned long)used).asCharString());
        CHECK_TEXT(used_in_callback < REENTRANT_DIRECTORY_OPERATION_STACK_BUDGET,
                   StringFromFormat("GetDirectory(\"..\") inside VisitDirectory used %lu bytes of stack", (unsigned long)used_in_callback).asCharString());
#endif
    }
}
