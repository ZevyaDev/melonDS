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

#ifndef HEADLESS_SUBS_H
#define HEADLESS_SUBS_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace Headless
{

class Instance;
struct AppliedInput;

struct SubRangeSpec
{
    uint8_t Cpu = 9;        // 7 or 9
    uint32_t Addr = 0;
    uint32_t Len = 0;
};

// Per-console frame subscriptions. The server thread edits them; the emulator thread calls
// Publish() at the end of every frame. A frame event (Protocol.h) is built only while the
// console has at least one subscription.
class Subscriptions
{
public:
    // Where finished frame events go: (console id, complete message). One sink for the
    // process; set by the server, null when it is not running.
    using Sink = std::function<void(int, std::vector<uint8_t>&&)>;
    static void SetSink(Sink sink);

    // Replace or add subscription `id`. False if the ranges are too large for one event.
    bool Set(uint32_t id, uint32_t everyN, std::vector<SubRangeSpec> ranges);
    bool Remove(uint32_t id);
    void Clear();
    bool Any() const { return Count.load() != 0; }

    // Emulator thread, after the frame ended. `applied` is what that frame applied.
    void Publish(Instance& inst, const AppliedInput& applied);

private:
    struct Sub
    {
        uint32_t Id;
        uint32_t Every;
        std::vector<SubRangeSpec> Ranges;
    };

    std::mutex Lock;
    std::vector<Sub> List;
    std::atomic<size_t> Count {0};
};

}

#endif
