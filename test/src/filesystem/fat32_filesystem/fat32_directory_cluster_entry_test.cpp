// Copyright 2026 GitHub Copilot.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "../../cpputest_support.h"

#include "filesystem/fat32_directory_cluster.h"
#include "filesystem/fat32_filenames.h"

namespace
{
    using namespace filesystems;
    using namespace filesystems::fat32;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (FAT32DirectoryClusterEntry)
    {
    };
#pragma GCC diagnostic pop

    TEST(FAT32DirectoryClusterEntry, Compact8Dot3Filename)
    {
        FAT32DirectoryClusterEntry entry("TEST    ",
                                         "TXT",
                                         FAT32DirectoryEntryAttributeFlags::FAT32DirectoryEntryAttributeFile,
                                         0,
                                         FAT32TimeHundredths(0),
                                         FAT32Time(0, 0, 0),
                                         FAT32Date(1980, 1, 1),
                                         FAT32Date(1980, 1, 1),
                                         FAT32ClusterIndex(2),
                                         FAT32Time(0, 0, 0),
                                         FAT32Date(1980, 1, 1),
                                         0);

        minstd::fixed_string<32> compact_filename;
        entry.Compact8Dot3Filename(compact_filename);

        STRCMP_EQUAL("TEST.TXT", compact_filename.c_str());

        FAT32ShortFilename short_filename;
        entry.AsShortFilename(short_filename);

        STRCMP_EQUAL("TEST", short_filename.Name().c_str());
        STRCMP_EQUAL("TXT", short_filename.Extension().c_str());
        CHECK(!short_filename.NumericTail().has_value());
    }

    TEST(FAT32DirectoryClusterEntry, VolumeLabel)
    {
        FAT32DirectoryClusterEntry entry("VOLLABEL ",
                                         "   ",
                                         FAT32DirectoryEntryAttributeFlags::FAT32DirectoryEntryAttributeVolumeId,
                                         0,
                                         FAT32TimeHundredths(0),
                                         FAT32Time(0, 0, 0),
                                         FAT32Date(1980, 1, 1),
                                         FAT32Date(1980, 1, 1),
                                         FAT32ClusterIndex(0),
                                         FAT32Time(0, 0, 0),
                                         FAT32Date(1980, 1, 1),
                                         0);

        minstd::fixed_string<32> volume_label;
        entry.VolumeLabel(volume_label);

        STRCMP_EQUAL("VOLLABEL", volume_label.c_str());
    }
}
