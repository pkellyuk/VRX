// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Bounded, process-owned diagnostics. No frame pixels or pointers cross IPC.
#pragma once
#include "interceptor.h"
#include "../xgstereo/pair_transport.h"
#include <type_traits>
namespace vrx::dx12 {
struct StatusFrame {
    uint32_t magic=0x53445856,version=1,bytes=sizeof(StatusFrame),pid=0;
    uint64_t updatedMs=0;
    uint32_t mirroring=0,reserved=0;
    Counts counts{};
};
static_assert(std::is_trivially_copyable_v<StatusFrame>,"diagnostic IPC is a flat snapshot");
inline std::wstring StatusName(const std::wstring& session,const wchar_t* suffix){return L"Local\\VRX.dx12."+session+suffix;}
class StatusMapping {
protected:
    HANDLE mutex_=nullptr,mapping_=nullptr;
    StatusFrame* view_=nullptr;
public:
    StatusMapping()=default;StatusMapping(const StatusMapping&)=delete;StatusMapping& operator=(const StatusMapping&)=delete;
    ~StatusMapping(){if(view_)UnmapViewOfFile(view_);if(mapping_)CloseHandle(mapping_);if(mutex_)CloseHandle(mutex_);}
};
class StatusWriter:public StatusMapping {
    bool mirroring_=false;
public:
    bool Create(const std::wstring& session,bool mirroring=false){
        if(!xg::ValidSession(session) || mutex_ || mapping_)return false;
        mutex_=CreateMutexW(nullptr,FALSE,StatusName(session,L".lock").c_str());if(!mutex_)return false;
        mapping_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(StatusFrame),StatusName(session,L".status").c_str());
        if(!mapping_ || GetLastError()==ERROR_ALREADY_EXISTS)return false;
        view_=static_cast<StatusFrame*>(MapViewOfFile(mapping_,FILE_MAP_WRITE,0,0,sizeof(StatusFrame)));if(!view_)return false;
        xg::PairLock lock(mutex_,1000);if(!lock.Ready())return false;
        mirroring_=mirroring;StatusFrame initial;initial.pid=GetCurrentProcessId();initial.mirroring=mirroring_;*view_=initial;return true;
    }
    bool Publish(const Counts& snapshot){
        xg::PairLock lock(mutex_);if(!lock.Ready() || !view_ || view_->pid!=GetCurrentProcessId())return false;
        StatusFrame frame;frame.pid=GetCurrentProcessId();frame.updatedMs=GetTickCount64();frame.mirroring=mirroring_;frame.counts=snapshot;*view_=frame;return true;
    }
};
class StatusReader:public StatusMapping {
public:
    bool Open(const std::wstring& session){
        if(!xg::ValidSession(session) || mutex_ || mapping_)return false;
        mutex_=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,StatusName(session,L".lock").c_str());
        mapping_=OpenFileMappingW(FILE_MAP_READ,FALSE,StatusName(session,L".status").c_str());if(!mutex_ || !mapping_)return false;
        view_=static_cast<StatusFrame*>(MapViewOfFile(mapping_,FILE_MAP_READ,0,0,sizeof(StatusFrame)));return view_!=nullptr;
    }
    bool Read(StatusFrame& out,uint32_t expectedPid=0){
        xg::PairLock lock(mutex_,100);if(!lock.Ready() || !view_)return false;
        const auto frame=*view_;
        if(frame.magic!=0x53445856 || frame.version!=1 || frame.bytes!=sizeof(frame) || !frame.pid || !frame.updatedMs || frame.mirroring>1 || frame.reserved ||
            frame.counts.bytes!=sizeof(Counts) || frame.counts.version!=4 || frame.counts.reason[sizeof(frame.counts.reason)-1] ||
            frame.updatedMs>GetTickCount64() || (expectedPid && frame.pid!=expectedPid))return false;
        out=frame;return true;
    }
};
} // namespace vrx::dx12
