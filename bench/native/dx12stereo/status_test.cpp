// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
#include "status.h"
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <future>
static void Check(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
int main(){try {
    const auto session=L"native-status-"+std::to_wstring(GetCurrentProcessId());vrx::dx12::StatusWriter writer;
    Check(writer.Create(session),"create bounded status channel");vrx::dx12::StatusReader reader;
    Check(reader.Open(session),"open status without loading the bridge or a graphics device");vrx::dx12::StatusFrame snapshot;
    Check(!reader.Read(snapshot),"initial unpopulated counts are not advertised as current");
    {vrx::dx12::StatusWriter duplicate;Check(!duplicate.Create(session),"another writer cannot replace a live status channel");}
    vrx::dx12::Counts counts;counts.draws=42;counts.mirrorStops=1;strcpy_s(counts.reason,"unsupported native call");counts.reason[sizeof(counts.reason)-1]='x';Check(writer.Publish(counts),"publish whole diagnostic snapshot with unused trailing bytes");
    Check(reader.Read(snapshot,GetCurrentProcessId()) && snapshot.counts.draws==42 && !std::strcmp(snapshot.counts.reason,counts.reason),"counts and stop reason read atomically with the owning PID");
    Check(!reader.Read(snapshot,GetCurrentProcessId()+1),"wrong game PID rejected");
    const auto valid=counts;std::memset(counts.reason,'x',sizeof(counts.reason));Check(writer.Publish(counts) && !reader.Read(snapshot),"unterminated diagnostic strings are rejected");counts=valid;
    HANDLE mutex=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,vrx::dx12::StatusName(session,L".lock").c_str());Check(mutex!=nullptr,"open contention fixture");
    std::promise<void> held,release;auto done=release.get_future();std::thread owner([&]{xg::PairLock lock(mutex,1000);held.set_value();done.wait();});held.get_future().wait();
    const auto before=GetTickCount64();const bool dropped=!writer.Publish(counts);const auto elapsed=GetTickCount64()-before;release.set_value();owner.join();CloseHandle(mutex);
    Check(dropped && elapsed<100,"status never stalls the game on a reader");Check(writer.Publish(counts) && reader.Read(snapshot),"status recovers from metadata contention");
    std::puts("PASS: native status ownership, atomic counts/reason, duplicate rejection and nonblocking game updates");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
