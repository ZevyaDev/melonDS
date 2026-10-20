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

#include <algorithm>

#include "Input.h"

namespace Headless
{

void InputQueue::SetWake(std::function<void()> wake)
{
    Wake = std::move(wake);
}

void InputQueue::WakeUp()
{
    if (Wake)
        Wake();
}

std::string InputQueue::Validate(const InputPayload& p)
{
    if (p.Touch.size() > InputMaxTouchPoints)
        return "bad_payload: more than 16 touch points";
    for (size_t i = 0; i < p.Touch.size(); i++)
    {
        const TouchPoint& t = p.Touch[i];
        if (t.Offset >= InputFrameCycles)
            return "bad_payload: touch offset out of range";
        if (i && t.Offset <= p.Touch[i - 1].Offset)
            return "bad_payload: touch offsets must increase";
        if (t.X > 255 || t.Y > 191)
            return "bad_payload: touch coordinates out of range";
    }
    if (p.Mic.size() > InputMaxMicSamples)
        return "bad_payload: too many microphone samples";
    if (p.MicKind > 2)
        return "bad_payload: unknown microphone kind";
    if (p.Buttons & ~0xFFF)
        return "bad_payload: unknown button bits";
    return {};
}

// The state once p has been applied. A payload without touch points leaves the pen as it was.
InputState InputQueue::After(const InputPayload& p, const InputState& before)
{
    InputState s = before;
    s.Buttons = p.Buttons;
    s.Lid = p.Lid;
    if (!p.Touch.empty())
    {
        const TouchPoint& t = p.Touch.back();
        s.TouchDown = t.Down;
        s.TouchX = t.X;
        s.TouchY = t.Y;
    }
    return s;
}

int InputQueue::Submit(InputPayload payload, std::string& error)
{
    error = Validate(payload);
    if (!error.empty())
        return -1;
    int depth;
    {
        std::lock_guard lock(Lock);
        if (Queue.size() >= InputQueueCapacity)
        {
            error = "input_queue_full";
            return -1;
        }
        Queue.push_back(std::move(payload));
        depth = (int)Queue.size();
    }
    WakeUp();
    return depth;
}

bool SubmitGroup(std::vector<GroupEntry>& entries, std::vector<int>& depths, std::string& error,
                 BarrierGroups& groups)
{
    depths.clear();
    if (entries.empty())
    {
        error = "bad_payload: no entries";
        return false;
    }
    std::vector<int> members;
    for (size_t i = 0; i < entries.size(); i++)
    {
        for (size_t j = 0; j < i; j++)
            if (entries[j].Console == entries[i].Console || entries[j].Queue == entries[i].Queue)
            {
                error = "bad_payload: console listed twice";
                return false;
            }
        error = InputQueue::Validate(entries[i].Payload);
        if (!error.empty())
            return false;
        members.push_back(entries[i].Console);
    }

    // Lock the queues in address order so two groups submitted at once cannot deadlock. The group is
    // created first: the registry must never be entered with a queue lock held.
    uint32_t id = groups.Create(members);
    std::vector<InputQueue*> order;
    for (GroupEntry& e : entries)
        order.push_back(e.Queue);
    std::sort(order.begin(), order.end());
    for (InputQueue* q : order)
        q->Lock.lock();

    bool full = false;
    for (GroupEntry& e : entries)
        if (e.Queue->Queue.size() >= InputQueueCapacity)
            full = true;
    if (!full)
    {
        for (GroupEntry& e : entries)
        {
            e.Payload.Barrier = id;
            e.Payload.Setter = false;
            e.Queue->Queue.push_back(std::move(e.Payload));
            depths.push_back((int)e.Queue->Queue.size());
        }
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        (*it)->Lock.unlock();

    if (full)
    {
        groups.Cancel(id);
        error = "input_queue_full";
        return false;
    }
    for (GroupEntry& e : entries)
        e.Queue->WakeUp();
    error.clear();
    return true;
}

int InputQueue::Modify(const std::function<void(InputPayload&)>& fn, std::string& error)
{
    int depth;
    {
        std::lock_guard lock(Lock);
        InputState base = Last;
        for (const InputPayload& q : Queue)
            base = After(q, base);

        InputPayload p;
        p.Source = InputSource::Local;
        p.Setter = true;
        p.Buttons = base.Buttons;
        p.Lid = base.Lid;
        p.Touch.push_back({0, base.TouchX, base.TouchY, base.TouchDown});
        fn(p);
        error = Validate(p);
        if (!error.empty())
            return -1;

        if (!Queue.empty() && Queue.back().Setter)
            Queue.back() = std::move(p);
        else if (Queue.size() >= InputQueueCapacity)
        {
            error = "input_queue_full";
            return -1;
        }
        else
            Queue.push_back(std::move(p));
        depth = (int)Queue.size();
    }
    WakeUp();
    return depth;
}

size_t InputQueue::Depth() const
{
    std::lock_guard lock(Lock);
    return Queue.size();
}

void InputQueue::SetBarrierOwner(int console, BarrierGroups* groups)
{
    std::lock_guard lock(Lock);
    BarrierOwner = console;
    Groups = groups ? groups : &Barriers();
}

uint32_t InputQueue::FrontBarrier() const
{
    std::lock_guard lock(Lock);
    return Queue.empty() ? 0 : Queue.front().Barrier;
}

std::vector<uint32_t> InputQueue::TakeBarriers()
{
    std::vector<uint32_t> out;
    if (BarrierOwner >= 0)
        for (const InputPayload& p : Queue)
            if (p.Barrier)
                out.push_back(p.Barrier);
    return out;
}

// Outside the queue's lock: this wakes other consoles.
void InputQueue::LeaveGroups(const std::vector<uint32_t>& groups)
{
    for (uint32_t g : groups)
        Groups->Drop(g, BarrierOwner);
}

void InputQueue::Clear()
{
    std::vector<uint32_t> dropped;
    {
        std::lock_guard lock(Lock);
        dropped = TakeBarriers();
        Queue.clear();
    }
    LeaveGroups(dropped);
    WakeUp();
}

void InputQueue::Reset()
{
    std::vector<uint32_t> dropped;
    {
        std::lock_guard lock(Lock);
        dropped = TakeBarriers();
        Queue.clear();
        Log.clear();
        LogBytes = 0;
        Last = {};
        LastSource = InputSource::Local;
        EmptyFrames = 0;
    }
    LeaveGroups(dropped);
    WakeUp();
}

void InputQueue::SetMode(EmptyMode mode)
{
    {
        std::lock_guard lock(Lock);
        Mode_ = mode;
    }
    WakeUp();
}

void InputQueue::SetStallTimeout(int frames)
{
    std::lock_guard lock(Lock);
    Stall = std::max(frames, 0);
}

EmptyMode InputQueue::Mode() const
{
    std::lock_guard lock(Lock);
    return Mode_;
}

int InputQueue::StallTimeout() const
{
    std::lock_guard lock(Lock);
    return Stall;
}

bool InputQueue::Next(uint64_t frame, AppliedInput& out)
{
    std::lock_guard lock(Lock);
    out = AppliedInput();

    if (!Queue.empty())
    {
        InputPayload p = std::move(Queue.front());
        Queue.pop_front();

        Last = After(p, Last);
        LastSource = p.Source;
        EmptyFrames = 0;

        out.Consumed = true;
        out.Barrier = p.Barrier;
        out.Seq = p.Seq;
        out.Source = p.Source;
        out.Buttons = p.Buttons;
        out.Lid = p.Lid;
        InputLogEntry e;
        e.Touch = p.Touch;
        e.Mic = p.Mic;
        out.Touch = std::move(p.Touch);
        if (out.Touch.empty())
            out.Touch.push_back({0, Last.TouchX, Last.TouchY, Last.TouchDown});
        out.MicKind = p.MicKind;
        out.Mic = std::move(p.Mic);
        out.MicLevel = p.MicLevel;

        e.Frame = frame;
        e.Seq = out.Source == InputSource::Client ? out.Seq : 0;
        e.Buttons = out.Buttons;
        e.Lid = out.Lid;
        e.TouchCount = (uint8_t)std::min<size_t>(out.Touch.size(), 255);
        e.MicKind = out.MicKind;
        e.MicLevel = out.MicLevel;
        LogBytes += e.Touch.size() * sizeof(TouchPoint) + e.Mic.size() * sizeof(int16_t);
        Log.push_back(std::move(e));
        while (Log.size() > InputLogCapacity || LogBytes > InputLogMaxBytes)
        {
            const InputLogEntry& old = Log.front();
            LogBytes -= old.Touch.size() * sizeof(TouchPoint) + old.Mic.size() * sizeof(int16_t);
            Log.pop_front();
        }
        return true;
    }

    if (Mode_ == EmptyMode::Wait)
        return false;

    // Empty frames 1..Stall still repeat; the one after that releases.
    EmptyFrames++;
    bool release = Mode_ == EmptyMode::Neutral;
    // Only a socket client can freeze while holding a button; in-process callers hold their state.
    if (Mode_ == EmptyMode::Repeat && Stall > 0 && LastSource == InputSource::Client && EmptyFrames > Stall)
        release = true;
    if (release)
    {
        // A closed lid is not "input", so it stays as it was.
        Last.Buttons = 0;
        Last.TouchDown = 0;
    }

    out.Source = LastSource;
    out.Buttons = Last.Buttons;
    out.Lid = Last.Lid;
    out.Touch.push_back({0, Last.TouchX, Last.TouchY, Last.TouchDown});
    return true;
}

InputState InputQueue::LastState() const
{
    std::lock_guard lock(Lock);
    return Last;
}

InputQueue::Held InputQueue::GetHeld() const
{
    std::lock_guard lock(Lock);
    return {Last, LastSource, EmptyFrames};
}

void InputQueue::SetHeld(const Held& held)
{
    std::lock_guard lock(Lock);
    Last = held.Last;
    LastSource = held.LastSource;
    EmptyFrames = held.EmptyFrames;
}

InputState InputQueue::NewestState() const
{
    std::lock_guard lock(Lock);
    InputState s = Last;
    for (const InputPayload& q : Queue)
        s = After(q, s);
    return s;
}

void TouchTimeline::Reset()
{
    Points.clear();
    Initial = {};
    Final = {};
}

void TouchTimeline::Begin(const std::vector<TouchPoint>& points)
{
    Points = points;
    Initial = Final;
    if (!Points.empty())
    {
        const TouchPoint& t = Points.back();
        Final = {t.Down != 0, t.X, t.Y};
    }
}

void TouchTimeline::FinalState(bool& down, uint16_t& x, uint16_t& y) const
{
    down = Final.Down;
    x = Final.X;
    y = Final.Y;
}

void TouchTimeline::Restore(bool down, uint16_t x, uint16_t y)
{
    Points.clear();
    Final = {down, x, y};
    Initial = Final;
}

bool TouchTimeline::StateAt(uint32_t offset, uint16_t& x, uint16_t& y) const
{
    const State* s = &Initial;
    State cur;
    for (const TouchPoint& t : Points)
    {
        if (t.Offset > offset)
            break;
        cur = {t.Down != 0, t.X, t.Y};
        s = &cur;
    }
    x = s->X;
    y = s->Y;
    return s->Down;
}

bool TouchTimeline::TouchAt(melonDS::u64 cyclesSinceFrameStart, melonDS::u16& x, melonDS::u16& y)
{
    uint32_t offset = cyclesSinceFrameStart > UINT32_MAX ? UINT32_MAX : (uint32_t)cyclesSinceFrameStart;
    return StateAt(offset, x, y);
}

std::vector<InputLogEntry> InputQueue::ReadLog(uint64_t fromFrame, size_t max) const
{
    std::lock_guard lock(Lock);
    std::vector<InputLogEntry> out;
    auto it = std::lower_bound(Log.begin(), Log.end(), fromFrame,
        [](const InputLogEntry& e, uint64_t f) { return e.Frame < f; });
    for (; it != Log.end() && out.size() < max; ++it)
        out.push_back(*it);
    return out;
}

}
