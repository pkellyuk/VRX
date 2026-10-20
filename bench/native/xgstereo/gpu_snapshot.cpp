// Diagnostic readback only: inspect an actual producer, independent of OpenXR.
#include "gpu_test_support.h"
#include <cstdio>
#include <memory>
namespace {
struct Peek {
    HANDLE mutex=nullptr,mapping=nullptr;const xg::GpuHeader* view=nullptr;
    ~Peek(){if(view)UnmapViewOfFile(view);if(mapping)CloseHandle(mapping);if(mutex)CloseHandle(mutex);}
};
}
int wmain(int argc, wchar_t** argv) {
    try {
        if ((argc!=3 && argc!=4) || !xg::ValidSession(argv[1])) throw std::runtime_error("usage: gpu_snapshot session output.ppm [expected-pid]");
        uint32_t expectedPid=0;if(argc==4){wchar_t* end=nullptr;const auto value=wcstoull(argv[3],&end,10);if(!end || *end || !value || value>UINT32_MAX)throw std::runtime_error("invalid expected PID");expectedPid=uint32_t(value);}
        xg::GpuHeader h{};
        {Peek peek;peek.mutex=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,xg::GpuName(argv[1],L".lock").c_str());peek.mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,xg::GpuName(argv[1],L".frames").c_str());if(!peek.mutex || !peek.mapping)throw std::runtime_error("no producer metadata");
            peek.view=static_cast<const xg::GpuHeader*>(MapViewOfFile(peek.mapping,FILE_MAP_READ,0,0,sizeof(xg::GpuHeader)));if(!peek.view)throw std::runtime_error("cannot read metadata");xg::PairLock lock(peek.mutex,1000);if(!lock.Ready())throw std::runtime_error("producer metadata is busy");h=*peek.view;
            if(!xg::ValidGpuHeader(h) || !h.active || (expectedPid && h.pid!=expectedPid))throw std::runtime_error("invalid, closed or foreign producer metadata");
        }
        xg::gpu_test::Snapshot snapshot; snapshot.Init(h.adapter,h.width,h.height,argv[1]);
        if(snapshot.reader.Header().pid!=h.pid || snapshot.reader.Header().generation!=h.generation)throw std::runtime_error("producer changed during snapshot setup");
        xg::PairFrame pair; const auto deadline=GetTickCount64()+5000;
        while (!snapshot.Read(pair) && GetTickCount64()<deadline) Sleep(1);
        if (pair.rgba.empty()) throw std::runtime_error("no completed pair");
        if(pair.header.eyeWidth!=h.width || pair.header.eyeHeight!=h.height || pair.rgba.size()!=size_t(h.width)*h.height*8)throw std::runtime_error("snapshot extent changed");
        FILE* f=nullptr; if (_wfopen_s(&f,argv[2],L"wb") || !f) throw std::runtime_error("cannot write diagnostic image");
        std::unique_ptr<FILE,decltype(&std::fclose)> file(f,&std::fclose);
        std::fprintf(f,"P6\n%u %u\n255\n",h.width*2,h.height);
        uint64_t nonblack[2]{},diff=0;
        for (uint32_t y=0;y<h.height;++y) for (uint32_t x=0;x<h.width*2;++x) {
            const auto* p=pair.rgba.data()+(size_t(y)*h.width*2+x)*4;
            std::fwrite(p,1,3,f); if (p[0]||p[1]||p[2]) ++nonblack[x/h.width];
            if (x<h.width) { const auto* q=p+h.width*4; for(uint32_t c=0;c<3;++c) diff+=uint64_t(std::abs(int(p[c])-int(q[c]))); }
        }
        if(std::ferror(f))throw std::runtime_error("diagnostic image write failed");
        file.release();if(std::fclose(f))throw std::runtime_error("diagnostic image close failed");
        std::printf("%ux%u per eye; frame %llu/%llu; nonblack %llu/%llu; mean eye difference %.6f\n",h.width,h.height,
            (unsigned long long)pair.header.leftFrame,(unsigned long long)pair.header.rightFrame,
            (unsigned long long)nonblack[0],(unsigned long long)nonblack[1],double(diff)/double(uint64_t(h.width)*h.height*3));
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
