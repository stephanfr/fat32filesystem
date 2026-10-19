// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include <stddef.h>
#include <stdint.h>

//
//  Peak stack use of a call, measured the way an RTOS measures a task's high-water mark: paint the
//      stack below the caller's frame with a pattern, make the call, then find the deepest byte it
//      changed.
//
//  Make all three calls from the same function, one after another, so they start from the same stack pointer:
//
//          ut_utility::PaintStack();
//          operation();
//          const size_t used = ut_utility::StackBytesUsedSincePaint();
//
//  Under AddressSanitizer, frames may live on a "fake stack" and the figure is meaningless.
//

namespace ut_utility
{
    constexpr size_t STACK_PAINT_BYTES = 128 * 1024;
    constexpr uint8_t STACK_PAINT_PATTERN = 0xA5;

    __attribute__((noinline, noclone)) inline void PaintStack()
    {
        volatile uint8_t region[STACK_PAINT_BYTES];

        for (size_t i = 0; i < STACK_PAINT_BYTES; i++)
        {
            region[i] = STACK_PAINT_PATTERN;
        }
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

    //  Reads what the measured call left behind: this region is deliberately uninitialised, and
    //      volatile, so every byte really is loaded.

    __attribute__((noinline, noclone)) inline size_t StackBytesUsedSincePaint()
    {
        volatile uint8_t region[STACK_PAINT_BYTES];

        //  The stack grows down, so region[0] is the deepest byte.  Count the painted bytes still
        //      intact from there; everything above them was used.

        size_t untouched = 0;

        while ((untouched < STACK_PAINT_BYTES) && (region[untouched] == STACK_PAINT_PATTERN))
        {
            untouched++;
        }

        return STACK_PAINT_BYTES - untouched;
    }

#pragma GCC diagnostic pop
} // namespace ut_utility
