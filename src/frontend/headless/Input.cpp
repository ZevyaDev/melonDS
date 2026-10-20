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

void InputQueue::Clear()
{
    {
        std::lock_guard lock(Lock);
        Queue.clear();
    }
    WakeUp();
}

void InputQueue::Reset()
{
    {
        std::lock_guard lock(Lock);
        Queue.clear();
        Log.clear();
        Last = {};
        LastSource = InputSource::Local;
        EmptyFrames = 0;
    }
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
        out.Seq = p.Seq;
        out.Source = p.Source;
        out.Buttons = p.Buttons;
        out.Lid = p.Lid;
        out.Touch = std::move(p.Touch);
        if (out.Touch.empty())
            out.Touch.push_back({0, Last.TouchX, Last.TouchY, Last.TouchDown});
        out.MicKind = p.MicKind;
        out.Mic = std::move(p.Mic);
        out.MicLevel = p.MicLevel;

        InputLogEntry e;
        e.Frame = frame;
        e.Seq = out.Source == InputSource::Client ? out.Seq : 0;
        e.Buttons = out.Buttons;
        e.Lid = out.Lid;
        e.TouchCount = (uint8_t)std::min<size_t>(out.Touch.size(), 255);
        e.MicKind = out.MicKind;
        e.MicLevel = out.MicLevel;
        if (Log.size() >= InputLogCapacity)
            Log.pop_front();
        Log.push_back(e);
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
