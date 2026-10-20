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

#include "Barrier.h"

namespace Headless
{

void BarrierGroups::Register(int console, WakeFn wake)
{
    std::lock_guard lock(Lock);
    Wakes[console] = std::move(wake);
}

void BarrierGroups::Unregister(int console)
{
    std::lock_guard lock(Lock);
    Wakes.erase(console);
    RecvPending.erase(console);
}

uint32_t BarrierGroups::Create(const std::vector<int>& members)
{
    std::lock_guard lock(Lock);
    uint32_t id = NextId++;
    if (!NextId)
        NextId = 1;
    Group& g = Groups[id];
    for (int m : members)
        g.Members[m] = State::Pending;
    return id;
}

void BarrierGroups::WakeArrived(const Group& g, int except)
{
    for (const auto& [console, state] : g.Members)
    {
        if (console == except || state != State::Arrived)
            continue;
        auto w = Wakes.find(console);
        if (w != Wakes.end() && w->second)
            w->second();
    }
}

// A group nobody is waiting for any more is forgotten.
void BarrierGroups::Finish(std::map<uint32_t, Group>::iterator it)
{
    for (const auto& m : it->second.Members)
        if (m.second == State::Pending || m.second == State::Arrived)
            return;
    Groups.erase(it);
}

void BarrierGroups::Cancel(uint32_t group)
{
    std::lock_guard lock(Lock);
    auto it = Groups.find(group);
    if (it == Groups.end())
        return;
    WakeArrived(it->second, -1);
    Groups.erase(it);
}

void BarrierGroups::Arrive(uint32_t group, int console)
{
    std::lock_guard lock(Lock);
    auto it = Groups.find(group);
    if (it == Groups.end())
        return;
    auto m = it->second.Members.find(console);
    if (m == it->second.Members.end() || m->second != State::Pending)
        return;
    m->second = State::Arrived;
    WakeArrived(it->second, console);
}

bool BarrierGroups::Released(uint32_t group, int console)
{
    std::lock_guard lock(Lock);
    auto it = Groups.find(group);
    if (it == Groups.end())
        return true;
    auto me = it->second.Members.find(console);
    if (me == it->second.Members.end() || me->second == State::Applied || me->second == State::Gone)
        return true;

    bool anyPending = false;
    for (const auto& [other, state] : it->second.Members)
    {
        if (state == State::Pending)
            anyPending = true;
        if (other != console && state != State::Gone)
        {
            auto r = RecvPending.find(other);
            if (r != RecvPending.end() && r->second > 0)
                return true;
        }
    }
    return !anyPending;
}

void BarrierGroups::Applied(uint32_t group, int console)
{
    std::lock_guard lock(Lock);
    auto it = Groups.find(group);
    if (it == Groups.end())
        return;
    auto m = it->second.Members.find(console);
    if (m == it->second.Members.end() || m->second == State::Gone)
        return;
    m->second = State::Applied;
    Finish(it);
}

void BarrierGroups::DropLocked(std::map<uint32_t, Group>::iterator it, int console)
{
    auto m = it->second.Members.find(console);
    if (m == it->second.Members.end() || m->second == State::Gone || m->second == State::Applied)
        return;
    m->second = State::Gone;
    WakeArrived(it->second, console);
    Finish(it);
}

void BarrierGroups::Drop(uint32_t group, int console)
{
    std::lock_guard lock(Lock);
    auto it = Groups.find(group);
    if (it != Groups.end())
        DropLocked(it, console);
}

void BarrierGroups::DropConsole(int console)
{
    std::lock_guard lock(Lock);
    for (auto it = Groups.begin(); it != Groups.end();)
    {
        auto cur = it++;        // DropLocked may erase the group
        DropLocked(cur, console);
    }
}

void BarrierGroups::SetRecvPending(int console, bool pending)
{
    std::lock_guard lock(Lock);
    int& count = RecvPending[console];
    count += pending ? 1 : -1;
    if (count <= 0)
        RecvPending.erase(console);
    if (!pending)
        return;
    for (const auto& [id, g] : Groups)
    {
        auto m = g.Members.find(console);
        if (m != g.Members.end() && m->second != State::Gone)
            WakeArrived(g, console);
    }
}

size_t BarrierGroups::GroupCount()
{
    std::lock_guard lock(Lock);
    return Groups.size();
}

BarrierGroups& Barriers()
{
    static BarrierGroups& groups = *new BarrierGroups();      // never destroyed, as the console registry
    return groups;
}

}
