// Copyright 2023 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include <stdint.h>
#include <algorithm>

#include "os_config.h"

//
//  FAT32 Dates and Times
//

namespace filesystems::fat32
{

    /**
     * @class FAT32Date
     * @brief Represents a date in the FAT32 file system format.
     *
     * The FAT32Date class provides methods to manipulate and retrieve date information in the FAT32 file system format.
     * It stores the year, month, and day as separate fields and provides methods to access and convert the date to the FAT32 format.
     */
    class FAT32Date
    {
    public:
        FAT32Date() = delete;
        FAT32Date(FAT32Date &&fat32_date) = delete;

        /**
         * @brief Constructs a FAT32Date object with the specified year, month, and day.
         *
         * @param year The year value. Must be greater than or equal to 1980.
         * @param month The month value. Must be between 1 and 12 (inclusive).
         * @param day The day value. Must be between 1 and 31 (inclusive).
         */
        FAT32Date(int year, int month, int day)
            : day_(minstd::min(minstd::max(day, 1), 31)),
              month_(minstd::min(minstd::max(month, 1), 12)),
              year_(minstd::min(minstd::max(0, year - 1980), 127))
        {
        }

        /**
         * @brief Copy constructor for FAT32Date.
         *
         * This constructor creates a new FAT32Date object by copying the values from another FAT32Date object.
         *
         * @param fat32_date The FAT32Date object to be copied.
         */
        FAT32Date(const FAT32Date &fat32_date) = default;

        /**
         * @brief Assignment operator for FAT32Date.
         *
         * This operator assigns the values of another FAT32Date object to the current object.
         *
         * @param fat32_date The FAT32Date object to copy from.
         * @return A reference to the current object after assignment.
         */
        FAT32Date &operator=(const FAT32Date &fat32_date) = default;

        FAT32Date &operator=(FAT32Date &&fat32_date) = delete;

        /**
         * @brief Returns the year value of the date.
         *
         * @return The year value of the date.
         */
        uint16_t Year() const noexcept
        {
            return year_ + 1980;
        }

        /**
         * @brief Returns the month value of the date.
         *
         * @return The month value of the date.
         */
        uint16_t Month() const noexcept
        {
            return month_;
        }

        /**
         * Retrieves the day value of the date.
         *
         * @return The day value of the date.
         */
        uint16_t Day() const noexcept
        {
            return day_;
        }

        /**
         * @brief Converts the date to FAT32 format.
         *
         * @return The date in FAT32 format.
         */
        uint16_t ToFAT32Date() const noexcept
        {
            return fat32_date_;
        }

    private:
        union
        {
            uint16_t fat32_date_;
            struct
            {
                uint16_t day_ : 5;   //  1-31
                uint16_t month_ : 4; //  1-12
                uint16_t year_ : 7;  //  0-127 (1980-2107)
            } PACKED;
        } PACKED;
    } PACKED;

    static_assert(sizeof(FAT32Date) == 2, "FAT32Date must match the 16-bit on-disk date field");

    /**
     * @class FAT32Time
     * @brief Represents a time value in the FAT32 file system.
     *
     * The FAT32Time class provides a way to store and manipulate time values in the FAT32 file system.
     * It supports hours, minutes, and seconds with a resolution of 2 seconds.
     *
     * The time values are stored in a packed union, so they map directly to the cluster layout.
     * The hours range from 0 to 23, the minutes range from 0 to 59, and the seconds range from 0 to 29 (0 to 59 in actual time).
     *
     * This class is used in the FAT32 directory cluster to represent the creation, modification, and access times of files and directories.
     */
    class FAT32Time
    {
    public:
        FAT32Time() = delete;
        FAT32Time(FAT32Time &&fat32_time) = delete;

        /**
         * @brief Constructs a FAT32Time object with the specified hours, minutes, and seconds.
         *
         * @param hours The hours value for the time (0-23).
         * @param minutes The minutes value for the time (0-59).
         * @param seconds The seconds value for the time (0-59, multiples of 2).
         */
        FAT32Time(int hours, int minutes, int seconds)
            : seconds_(minstd::min(minstd::max(seconds / 2, 0), 29)),
              minutes_(minstd::min(minstd::max(minutes, 0), 59)),
              hours_(minstd::min(minstd::max(hours, 0), 23))
        {
        }

        /**
         * @brief Constructs a FAT32Time object by copying another FAT32Time object.
         *
         * @param fat32_time The FAT32Time object to be copied.
         */
        FAT32Time(const FAT32Time &fat32_time) = default;

        /**
         * @brief Assignment operator for FAT32Time objects.
         *
         * This operator assigns the values of the given FAT32Time object to the current object.
         *
         * @param fat32_time The FAT32Time object to be assigned.
         * @return A reference to the current FAT32Time object after assignment.
         */
        FAT32Time &operator=(const FAT32Time &fat32_time) = default;

        FAT32Time &operator=(FAT32Time &&fat32_time) = delete;

        /**
         * Returns the hours component of the time.
         *
         * @return The hours component of the time.
         */
        uint16_t Hours() const noexcept
        {
            return hours_;
        }

        /**
         * @brief Returns the number of minutes.
         *
         * @return The number of minutes.
         */
        uint16_t Minutes() const noexcept
        {
            return minutes_;
        }

        /**
         * @brief Returns the number of seconds.
         *
         * @return The number of seconds.
         */
        uint16_t Seconds() const noexcept
        {
            return seconds_ * 2;
        }

        /**
         * @brief Converts the time to FAT32 on-disk format.
         *
         * @return The time in FAT32 format.
         */
        uint16_t ToFAT32Time() const noexcept
        {
            return fat32_time_;
        }

    private:
        union
        {
            uint16_t fat32_time_;
            struct
            {
                uint16_t seconds_ : 5; //  0-29 (0-59)
                uint16_t minutes_ : 6; //  0-59
                uint16_t hours_ : 5;   //  0-23
            } PACKED;
        } PACKED;
    } PACKED;

    static_assert(sizeof(FAT32Time) == 2, "FAT32Time must match the 16-bit on-disk time field");

    /**
     * @class FAT32TimeHundredths
     * @brief Represents the hundredths of a second in a FAT32 time value.
     *
     * This class provides a representation of the hundredths of a second in a FAT32 time value.
     * It is used to store and manipulate the hundredths component of a time value.
     *
     * The range of valid values for the hundredths component is from 0 to 199 so it can store two seconds of time.
     *
     * @note This class is not meant to be instantiated directly. Use the provided constructors and assignment operators.
     */
    class FAT32TimeHundredths
    {
    public:
        FAT32TimeHundredths() = delete;
        FAT32TimeHundredths(FAT32TimeHundredths &&fat32_time_hundredths) = delete;

        /**
         * @brief Constructs a FAT32TimeHundredths object with the specified hundredths value.
         *
         * @param hundredths The hundredths value to be set. Must be between 0 and 199 (inclusive).
         */
        FAT32TimeHundredths(int hundredths)
            : hundredths_(minstd::min(minstd::max(hundredths, 0), 199))
        {
        }

        /**
         * @brief Copy constructor for FAT32TimeHundredths.
         *
         * @param fat32_time_hundredths The FAT32TimeHundredths object to be copied.
         */
        FAT32TimeHundredths(const FAT32TimeHundredths &fat32_time_hundredths) = default;

        /**
         * @brief Assignment operator for FAT32TimeHundredths.
         *
         * This operator assigns the value of another FAT32TimeHundredths object to the current object.
         *
         * @param fat32_time_hundredths The FAT32TimeHundredths object to be assigned.
         * @return Reference to the current FAT32TimeHundredths object after assignment.
         */
        FAT32TimeHundredths &operator=(const FAT32TimeHundredths &fat32_time_hundredths) = default;

        FAT32TimeHundredths &operator=(FAT32TimeHundredths &&fat32_time_hundredths) = delete;

        /**
         * @brief Returns the value of the Hundredths field.
         *
         * @return The value of the Hundredths field.
         */
        uint16_t Hundredths() const noexcept
        {
            return hundredths_;
        }

    private:
        uint8_t hundredths_;
    } PACKED;

    static_assert(sizeof(FAT32TimeHundredths) == 1, "FAT32TimeHundredths must match the 8-bit on-disk field");
}
