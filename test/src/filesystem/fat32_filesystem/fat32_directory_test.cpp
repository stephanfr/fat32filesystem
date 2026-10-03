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

        CHECK_EQUAL(FilesystemResultCodes::DIRECTORY_NOT_FOUND, (*stale)->RemoveDirectory());

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

        minstd::fixed_string<MAX_FILESYSTEM_PATH_LENGTH> absolute_path("/rootfile.txt");

        CHECK(GetFileMap().IsFileOpen(absolute_path));

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
}
