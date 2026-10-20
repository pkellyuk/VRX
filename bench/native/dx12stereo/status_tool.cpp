// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
#include "status.h"
#include <cstdio>
#include <string>
static std::string Json(const char* text){std::string out;for(const unsigned char* c=reinterpret_cast<const unsigned char*>(text);*c;++c){
    if(*c=='"' || *c=='\\'){out+='\\';out+=char(*c);}else if(*c<32){char escaped[7]{};sprintf_s(escaped,"\\u%04x",*c);out+=escaped;}else out+=char(*c);
}return out;}
int wmain(int argc,wchar_t** argv){
    if(argc<2 || argc>3){std::fprintf(stderr,"usage: dx12_status SESSION [PID]\n");return 2;}
    uint32_t expected=0;if(argc==3){wchar_t* end=nullptr;const auto pid=wcstoull(argv[2],&end,10);if(!end || *end || !pid || pid>UINT32_MAX)return 2;expected=uint32_t(pid);}
    vrx::dx12::StatusReader reader;vrx::dx12::StatusFrame frame;
    if(!reader.Open(argv[1]) || !reader.Read(frame,expected)){std::fprintf(stderr,"Native DX12 status unavailable or owned by another process\n");return 1;}
    const auto& c=frame.counts;const auto age=GetTickCount64()-frame.updatedMs;
    std::printf("{\"pid\":%u,\"mirroring\":%s,\"ageMs\":%llu,\"devices\":%llu,\"queues\":%llu,\"resources\":%llu,\"pipelines\":%llu,\"streamPipelines\":%llu,\"memoryHeaps\":%llu,\"placedResources\":%llu,\"modernResources\":%llu,\"draws\":%llu,\"indexedDraws\":%llu,\"submissions\":%llu,\"hookFailures\":%llu,\"replays\":%llu,\"stops\":%llu,\"publications\":%llu,\"reason\":\"%s\"}\n",
        frame.pid,frame.mirroring?"true":"false",(unsigned long long)age,(unsigned long long)c.devices,(unsigned long long)c.queues,(unsigned long long)c.resources,(unsigned long long)c.pipelines,
        (unsigned long long)c.streamPipelines,(unsigned long long)c.memoryHeaps,(unsigned long long)c.placedResources,(unsigned long long)c.modernResources,
        (unsigned long long)c.draws,(unsigned long long)c.indexedDraws,(unsigned long long)c.submissions,(unsigned long long)c.hookFailures,
        (unsigned long long)c.mirrorReplays,(unsigned long long)c.mirrorStops,(unsigned long long)c.publications,Json(c.reason).c_str());return 0;
}
