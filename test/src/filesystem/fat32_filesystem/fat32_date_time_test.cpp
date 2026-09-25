// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "../../cpputest_support.h"

#include "filesystem/fat32_date_time.h"

namespace
{
    using namespace filesystems;
    using namespace filesystems::fat32;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
    TEST_GROUP (FAT32DateTime)
    {
    };
#pragma GCC diagnostic pop

    //
    //  Tests start below
    //

    TEST(FAT32DateTime, FAT32DateTest)
    {
        {
            FAT32Date date(1980, 1, 1);

            CHECK_EQUAL(1980, date.Year());
            CHECK_EQUAL(1, date.Month());
            CHECK_EQUAL(1, date.Day());
        }

        {
            FAT32Date date(2107, 12, 31);

            CHECK_EQUAL(2107, date.Year());
            CHECK_EQUAL(12, date.Month());
            CHECK_EQUAL(31, date.Day());
        }

        {
            FAT32Date date(1979, -1, 0);

            CHECK_EQUAL(1980, date.Year());
            CHECK_EQUAL(1, date.Month());
            CHECK_EQUAL(1, date.Day());
        }

        {
            FAT32Date date(2108, 13, 32);

            CHECK_EQUAL(2107, date.Year());
            CHECK_EQUAL(12, date.Month());
            CHECK_EQUAL(31, date.Day());
        }
    }

    TEST(FAT32DateTime, FAT32TimeTest)
    {
        {
            FAT32Time time(0, 0, 0);

            CHECK_EQUAL(0, time.Hours());
            CHECK_EQUAL(0, time.Minutes());
            CHECK_EQUAL(0, time.Seconds());
        }

        {
            FAT32Time time(23, 59, 59);

            CHECK_EQUAL(23, time.Hours());
            CHECK_EQUAL(59, time.Minutes());
            CHECK_EQUAL(58, time.Seconds());
        }

        {
            FAT32Time time(-1, -1, -1);

            CHECK_EQUAL(0, time.Hours());
            CHECK_EQUAL(0, time.Minutes());
            CHECK_EQUAL(0, time.Seconds());
        }

        {
            FAT32Time time(24, 60, 60);

            CHECK_EQUAL(23, time.Hours());
            CHECK_EQUAL(59, time.Minutes());
            CHECK_EQUAL(58, time.Seconds());
        }
    }

    TEST(FAT32DateTime, FAT32TimeHundredthsTest)
    {
        {
            FAT32TimeHundredths hundredths(0);

            CHECK_EQUAL(0, hundredths.Hundredths());
        }

        {
            FAT32TimeHundredths hundredths(199);

            CHECK_EQUAL(199, hundredths.Hundredths());
        }

        {
            FAT32TimeHundredths hundredths(-1);

            CHECK_EQUAL(0, hundredths.Hundredths());
        }

        {
            FAT32TimeHundredths hundredths(200);

            CHECK_EQUAL(199, hundredths.Hundredths());
        }
    }

    //
    //  The three tests above round-trip through the accessors, so they pass whether or not the
    //      bitfield order matches the on-disk format.  The tests below pin the actual packed
    //      bytes, which is the contract that matters to every other FAT32 implementation.
    //

    TEST(FAT32DateTime, FAT32DateMatchesOnDiskLayout)
    {
        //  FAT32 spec: bits 0-4 day, bits 5-8 month, bits 9-15 year - 1980.

        CHECK_EQUAL(0x0021, FAT32Date(1980, 1, 1).ToFAT32Date());    //  (0 << 9)   | (1 << 5)  | 1
        CHECK_EQUAL(0x58E6, FAT32Date(2024, 7, 6).ToFAT32Date());    //  (44 << 9)  | (7 << 5)  | 6
        CHECK_EQUAL(0xFF9F, FAT32Date(2107, 12, 31).ToFAT32Date());  //  (127 << 9) | (12 << 5) | 31
    }

    TEST(FAT32DateTime, FAT32TimeMatchesOnDiskLayout)
    {
        //  FAT32 spec: bits 0-4 seconds/2, bits 5-10 minutes, bits 11-15 hours.

        CHECK_EQUAL(0x0000, FAT32Time(0, 0, 0).ToFAT32Time());
        CHECK_EQUAL(0x4ADA, FAT32Time(9, 22, 52).ToFAT32Time());     //  (9 << 11)  | (22 << 5) | 26
        CHECK_EQUAL(0xBF7D, FAT32Time(23, 59, 58).ToFAT32Time());    //  (23 << 11) | (59 << 5) | 29
    }

    TEST(FAT32DateTime, DecodesTimestampsWrittenByMkfs)
    {
        //  These two values are lifted verbatim from a directory entry in
        //      test/data/test_fat32.img and are what mkfs.fat actually wrote.
        //      With the bitfields reversed the time decodes to hours == 26.

        const uint16_t on_disk_date = 0x58E6;
        const uint16_t on_disk_time = 0x4ADA;

        const FAT32Date &date = reinterpret_cast<const FAT32Date &>(on_disk_date);
        const FAT32Time &time = reinterpret_cast<const FAT32Time &>(on_disk_time);

        CHECK_EQUAL(2024, date.Year());
        CHECK_EQUAL(7, date.Month());
        CHECK_EQUAL(6, date.Day());

        CHECK_EQUAL(9, time.Hours());
        CHECK_EQUAL(22, time.Minutes());
        CHECK_EQUAL(52, time.Seconds());
    }

    TEST(FAT32DateTime, PackedSizesMatchTheOnDiskRecord)
    {
        CHECK_EQUAL(2, sizeof(FAT32Date));
        CHECK_EQUAL(2, sizeof(FAT32Time));
        CHECK_EQUAL(1, sizeof(FAT32TimeHundredths));
    }
}
