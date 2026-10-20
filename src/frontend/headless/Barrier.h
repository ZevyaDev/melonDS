/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef HEADLESS_BARRIER_H
#define HEADLESS_BARRIER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace Headless
{

// Bookkeeping for input_multi barrier groups: pure logic, no console objects. A group is the set of
// consoles that were sent a payload together. Each member is Pending (its barrier payload is not
// next in its queue yet), Arrived (it is waiting at its frame boundary), Applied (it took its payload)
// or Gone (it can no longer arrive: destroyed, stopped, or its queue was cleared).
//
// A member may go ahead (Released) when it is Applied or Gone itself, or when no member is Pending, or
// when another member is blocked in a wireless receive (SetRecvPending): a console parked at the barrier
// cannot answer that partner's packets, so it is let go rather than make the partner time out.
//
// Locking: the wake callbacks run under the registry's lock, so a callback may take its own console's
// lock, but no caller may hold such a lock while calling in here.
class BarrierGroups
{
public:
    using WakeFn = std::function<void()>;

    // Called (under the lock) when something happened that may release this console while it waits.
    void Register(int console, WakeFn wake);
    void Unregister(int console);

    uint32_t Create(const std::vector<int>& members);       // a new nonzero group id
    void Cancel(uint32_t group);                            // forget a group whose payloads were never queued

    void Arrive(uint32_t group, int console);               // the console waits at its barrier payload
    bool Released(uint32_t group, int console);
    void Applied(uint32_t group, int console);              // it took its payload and starts the frame
    void Drop(uint32_t group, int console);                 // it can no longer arrive
    void DropConsole(int console);                          // ... in every group

    // The console entered (true) or left (false) a blocking wireless receive. Calls nest.
    void SetRecvPending(int console, bool pending);

    size_t GroupCount();

private:
    enum class State : uint8_t { Pending, Arrived, Applied, Gone };

    struct Group
    {
        std::map<int, State> Members;
    };

    std::mutex Lock;
    std::map<uint32_t, Group> Groups;
    std::map<int, WakeFn> Wakes;
    std::map<int, int> RecvPending;
    uint32_t NextId = 1;

    void WakeArrived(const Group& g, int except);
    void Finish(std::map<uint32_t, Group>::iterator it);
    void DropLocked(std::map<uint32_t, Group>::iterator it, int console);
};

// The process-wide registry used by the consoles.
BarrierGroups& Barriers();

}

#endif
