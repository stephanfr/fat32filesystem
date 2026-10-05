// Copyright 2024 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "filesystem/filesystem_path.h"
#include "heaps.h"

#include <ctype.h>
#include <string.h>

namespace filesystems
{
    PointerResult<FilesystemResultCodes, FilesystemPath> FilesystemPath::ParsePathString(const minstd::string &path_string)
    {
        using Result = PointerResult<FilesystemResultCodes, FilesystemPath>;

        //  First, insure the path string is superficially legit

        if (path_string.empty())
        {
            return Result::Failure(FilesystemResultCodes::EMPTY_PATH);
        }

        const unsigned char first_char = static_cast<unsigned char>(path_string[0]);

        //  A relative path may begin with any printable, non-blank character.  Filenames such as
        //      "_config" and "$data" are legal.

        if ((path_string[0] != DIRECTORY_DELIMITER) && (!isprint(first_char) || isspace(first_char) || (first_char == '.')))
        {
            return Result::Failure(FilesystemResultCodes::ILLEGAL_PATH);
        }

        if (isspace(static_cast<unsigned char>(path_string[path_string.length() - 1])))     //  Insure path does not end with whitespace
        {
            return Result::Failure(FilesystemResultCodes::ILLEGAL_PATH);
        }

        if (path_string.length() >= MAX_FILESYSTEM_PATH_LENGTH)
        {
            return Result::Failure(FilesystemResultCodes::PATH_TOO_LONG);
        }

        //  Start parsing

        minstd::unique_ptr<FilesystemPath> path(dynamic_new<FilesystemPath>(FilesystemPath(path_string)));

        //  Check if this is the trivial case of the root directory.  We know the first character is the root directory delimiter above.

        if ((path_string.length() == 1) && (path_string[0] == DIRECTORY_DELIMITER))
        {
            path->is_root_ = true;
            path->is_relative_ = false;

            path->parsed_path_[0] = 0x00;

            return Result::Success(minstd::move(path));
        }

        if (path_string[path_string.length() - 1] == DIRECTORY_DELIMITER)
        {
            return Result::Failure(FilesystemResultCodes::ILLEGAL_PATH);
        }

        //  Determine if this is a relative path or not

        path->is_relative_ = (path_string[0] != DIRECTORY_DELIMITER);

        //  Pass through the path string changing directory delimiters to nulls.
        //      Insure the path elements are sematically correct as we parse.

        for (uint32_t i = 0; i < path->length_; i++)
        {
            if (!isprint(static_cast<unsigned char>(path->parsed_path_[i])))
            {
                return Result::Failure(FilesystemResultCodes::ILLEGAL_PATH);
            }

            if (path->parsed_path_[i] == DIRECTORY_DELIMITER)
            {
                if ((i > 0) && (path->parsed_path_[i - 1] == 0))
                {
                    return Result::Failure(FilesystemResultCodes::ILLEGAL_PATH);
                }

                path->parsed_path_[i] = 0;
            }
        }

        //  Handle dot and dot-dot components as illegal

        for (auto itr = path->begin(); itr != path->end(); itr++)
        {
            if ((strncmp(*itr, ".", 2) == 0) || (strncmp(*itr, "..", 3) == 0))
            {
                return Result::Failure(FilesystemResultCodes::ILLEGAL_PATH);
            }
        }

        //  If we are down here, the path is legit

        return Result::Success(minstd::move(path));
    }
} // namespace filesystems
