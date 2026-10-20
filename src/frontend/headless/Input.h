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

#ifndef HEADLESS_INPUT_H
#define HEADLESS_INPUT_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "InputTimeline.h"

namespace melonDS { class NDS; }

namespace Headless
{

class Instance;

constexpr size_t InputQueueCapacity = 600;      // payloads per console (10 s at 60 fps)
constexpr size_t InputMaxTouchPoints = 16;
constexpr uint32_t InputFrameCycles = 560190;   // touch offsets are below this
constexpr size_t InputMaxMicSamples = 796;
constexpr size_t InputLogCapacity = 216000;     // one hour at 60 fps
constexpr int InputDefaultStallFrames = 15;

enum class InputSource : uint8_t
{
    Client,     // a socket client
    Local,      // in-process: the C API and Lua
};

enum class EmptyMode : uint8_t
{
    Repeat,
    Neutral,
    Wait,
};

struct TouchPoint
{
    uint32_t Offset = 0;    // system cycles from frame start
    uint16_t X = 0, Y = 0;
    uint8_t Down = 0;
};

struct InputPayload
{
    uint32_t Seq = 0;
    uint16_t Buttons = 0;   // bit per key, same order as mds_set_keys
    uint8_t Lid = 0;        // 1 = closed
    std::vector<TouchPoint> Touch;  // empty = no change from the last frame
    uint8_t MicKind = 0;    // 0 none, 1 samples, 2 level
    std::vector<int16_t> Mic;
    uint16_t MicLevel = 0;
    InputSource Source = InputSource::Client;
    uint32_t Barrier = 0;   // barrier group id, 0 = none (reserved for input_multi)
    bool Setter = false;    // from mds_set_keys/touch/lid: a later setter amends it while unconsumed
};

// What the emulator thread applies to one frame.
struct AppliedInput
{
    uint32_t Seq = 0;
    uint16_t Buttons = 0;
    uint8_t Lid = 0;
    std::vector<TouchPoint> Touch;  // never empty; the last point is the pen state at the end of the frame
    uint8_t MicKind = 0;
    std::vector<int16_t> Mic;
    uint16_t MicLevel = 0;
    bool Consumed = false;          // a queued payload was used (as opposed to the empty-queue result)
    InputSource Source = InputSource::Local;
};

struct InputState
{
    uint16_t Buttons = 0;
    uint8_t Lid = 0;
    uint8_t TouchDown = 0;
    uint16_t TouchX = 0, TouchY = 0;
};

struct InputLogEntry
{
    uint64_t Frame = 0;
    uint32_t Seq = 0;       // 0 for Local sources
    uint16_t Buttons = 0;
    uint8_t Lid = 0;
    uint8_t TouchCount = 0;
    uint8_t MicKind = 0;
    uint16_t MicLevel = 0;
};

// Per-console input FIFO. Submit() may be called from any thread; Next() belongs to the
// emulator thread, at the start of a frame.
class InputQueue
{
public:
    // Called after anything that can release a console waiting for input (a submit, a mode
    // change, a clear), on the calling thread, outside the queue's lock. Set before sharing.
    void SetWake(std::function<void()> wake);

    // Validate and append. Returns the new depth, or -1 with error set ("bad_payload: ...",
    // "input_queue_full"). Nothing is applied on failure.
    int Submit(InputPayload payload, std::string& error);

    // Build a Local setter payload from the newest state (the last queued payload, else the last
    // applied one), let fn change it, then amend the newest queued payload if that is also a
    // setter payload, else append. Returns the depth, or -1.
    int Modify(const std::function<void(InputPayload&)>& fn, std::string& error);

    size_t Depth() const;
    void Clear();                       // drop queued payloads
    void Reset();                       // Clear, plus the log and the held state (the console was reset)

    void SetMode(EmptyMode mode);
    void SetStallTimeout(int frames);   // repeat mode only; 0 = never
    EmptyMode Mode() const;
    int StallTimeout() const;

    // Resolve the input for the frame about to start. False only in Wait mode with an empty
    // queue: nothing is consumed and the caller must wait for a wake-up.
    bool Next(uint64_t frame, AppliedInput& out);

    InputState LastState() const;       // as applied
    InputState NewestState() const;     // as it will be once the queue drains

    std::vector<InputLogEntry> ReadLog(uint64_t fromFrame, size_t max) const;

private:
    mutable std::mutex Lock;
    std::deque<InputPayload> Queue;
    EmptyMode Mode_ = EmptyMode::Repeat;
    int Stall = InputDefaultStallFrames;
    InputState Last;
    InputSource LastSource = InputSource::Local;
    int EmptyFrames = 0;
    std::deque<InputLogEntry> Log;
    std::function<void()> Wake;

    static std::string Validate(const InputPayload& p);
    static InputState After(const InputPayload& p, const InputState& before);
    void WakeUp();
};

// The touch points of the frame being run, as the core's InputTimeline. Each point holds from its
// offset until the next; before the first one, and after the last one in later frames, the state
// the previous frame ended in holds. Emulator thread only.
class TouchTimeline : public melonDS::InputTimeline
{
public:
    void Reset();                                       // released, as after a console reset
    void Begin(const std::vector<TouchPoint>& points);  // a frame is about to start
    bool StateAt(uint32_t offset, uint16_t& x, uint16_t& y) const;
    bool TouchAt(melonDS::u64 cyclesSinceFrameStart, melonDS::u16& x, melonDS::u16& y) override;

private:
    struct State
    {
        bool Down = false;
        uint16_t X = 0, Y = 0;
    };

    std::vector<TouchPoint> Points;
    State Initial;      // before the first point of this frame
    State Final;        // after the last point; the next frame's Initial
};

// NDS-facing appliers (InputApply.cpp), run on the emulator thread at the start of a frame.
void ApplyButtons(melonDS::NDS& nds, const AppliedInput& in);
void ApplyLid(melonDS::NDS& nds, const AppliedInput& in);
void ApplyTouch(melonDS::NDS& nds, TouchTimeline& line, const AppliedInput& in);
void ApplyMic(Instance& inst, const AppliedInput& in);
void ApplyInput(Instance& inst, const AppliedInput& in);

}

#endif
