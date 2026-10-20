// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Verify the opaque-export ABI independently of undocumented Windows functions.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <exception>

extern "C" uint64_t D3D12PrivateOrdinal99(uint64_t,double,uint64_t,float,uint64_t,double,void*,uint64_t);
extern "C" double D3D12PIXReportCounter(double,uint64_t,float,uint64_t,double,uint64_t);
static bool valid=false;
static int token;
static uint64_t IntegerReturn(uint64_t a,double b,uint64_t c,float d,uint64_t e,double f,void* g,uint64_t h) {
    valid=a==0x1122334455667788ull && b==1.25 && c==0x8877665544332211ull && d==-.5f &&
        e==0xabcdef1234567890ull && f==9.5 && g==&token && h==0xfedcba0987654321ull;
    return 0x9182736455463728ull;
}
static double FloatReturn(double a,uint64_t b,float c,uint64_t d,double e,uint64_t f) {
    valid=a==1.5 && b==123 && c==-.25f && d==456 && e==7.25 && f==789;
    return -1234.125;
}
extern "C" FARPROC ResolveSystemOrdinal(UINT ordinal) noexcept {
    if(ordinal==99) return reinterpret_cast<FARPROC>(&IntegerReturn);
    if(ordinal==114) return reinterpret_cast<FARPROC>(&FloatReturn);
    std::terminate();
}
int main() {
    const auto integer=D3D12PrivateOrdinal99(0x1122334455667788ull,1.25,0x8877665544332211ull,-.5f,
        0xabcdef1234567890ull,9.5,&token,0xfedcba0987654321ull);
    if(!valid || integer!=0x9182736455463728ull) return 1;
    valid=false;
    const auto floating=D3D12PIXReportCounter(1.5,123,-.25f,456,7.25,789);
    if(!valid || floating!=-1234.125) return 1;
    std::puts("PASS: opaque Win64 forwarding preserves integer, floating-point and stack arguments and both return registers");
    return 0;
}
