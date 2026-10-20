// Diagnostic readback only: inspect an actual producer, independent of OpenXR.
#include "gpu_test_support.h"
#include <cstdio>
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=3 || !xg::ValidSession(argv[1])) throw std::runtime_error("usage: gpu_snapshot session output.ppm");
        const auto name=xg::GpuName(argv[1],L".frames");
        HANDLE mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,name.c_str()); if (!mapping) throw std::runtime_error("no producer metadata");
        auto* view=(const xg::GpuHeader*)MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(xg::GpuHeader));
        if (!view) { CloseHandle(mapping); throw std::runtime_error("cannot read metadata"); }
        const auto h=*view; UnmapViewOfFile(view); CloseHandle(mapping);
        if (!xg::ValidGpuHeader(h)) throw std::runtime_error("invalid producer metadata");
        xg::gpu_test::Snapshot snapshot; snapshot.Init(h.adapter,h.width,h.height,argv[1]);
        xg::PairFrame pair; const auto deadline=GetTickCount64()+5000;
        while (!snapshot.Read(pair) && GetTickCount64()<deadline) Sleep(1);
        if (pair.rgba.empty()) throw std::runtime_error("no completed pair");
        FILE* f=nullptr; if (_wfopen_s(&f,argv[2],L"wb") || !f) throw std::runtime_error("cannot write diagnostic image");
        std::fprintf(f,"P6\n%u %u\n255\n",h.width*2,h.height);
        uint64_t nonblack[2]{},diff=0;
        for (uint32_t y=0;y<h.height;++y) for (uint32_t x=0;x<h.width*2;++x) {
            const auto* p=pair.rgba.data()+(size_t(y)*h.width*2+x)*4;
            std::fwrite(p,1,3,f); if (p[0]||p[1]||p[2]) ++nonblack[x/h.width];
            if (x<h.width) { const auto* q=p+h.width*4; for(uint32_t c=0;c<3;++c) diff+=uint64_t(std::abs(int(p[c])-int(q[c]))); }
        }
        std::fclose(f);
        std::printf("%ux%u per eye; frame %llu/%llu; nonblack %llu/%llu; mean eye difference %.6f\n",h.width,h.height,
            (unsigned long long)pair.header.leftFrame,(unsigned long long)pair.header.rightFrame,
            (unsigned long long)nonblack[0],(unsigned long long)nonblack[1],double(diff)/double(uint64_t(h.width)*h.height*3));
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
