// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include <stdint.h>

extern uintptr_t g_test_task_owner_id;

//  Poses as a second task for the lifetime of the object.  Keep CHECKs out of its scope: a failed
//      CHECK longjmps past the destructor and the identity would stick.

class AsAnotherTask
{
public:
    AsAnotherTask()
        : saved_owner_id_(g_test_task_owner_id)
    {
        g_test_task_owner_id = saved_owner_id_ + 1;
    }

    ~AsAnotherTask()
    {
        g_test_task_owner_id = saved_owner_id_;
    }

    AsAnotherTask(const AsAnotherTask &) = delete;
    AsAnotherTask &operator=(const AsAnotherTask &) = delete;

private:
    const uintptr_t saved_owner_id_;
};

//  True if another task could take the lock right now - i.e. nobody holds it.

template <typename Mutex>
bool LockIsFree(Mutex &mutex)
{
    AsAnotherTask other_task;

    if (!mutex.try_lock())
    {
        return false;
    }

    mutex.unlock();

    return true;
}
