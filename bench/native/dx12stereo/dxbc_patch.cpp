// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Own token rewrite; enums and hashing are from Microsoft's public DXC source.
#include <windows.h>
#include "dxbc_patch.h"
#include "d3d12TokenizedProgramFormat.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <initializer_list>

void ComputeHashRetail(const BYTE*,UINT,BYTE*);
namespace vrx::dx12::dxbc {
namespace {
constexpr size_t MaxBytes=16*1024*1024;
constexpr uint32_t Tag(char a,char b,char c,char d) { return uint32_t(uint8_t(a))|(uint32_t(uint8_t(b))<<8)|(uint32_t(uint8_t(c))<<16)|(uint32_t(uint8_t(d))<<24); }
uint32_t U(const uint8_t* p) { uint32_t value; std::memcpy(&value,p,4); return value; }
void W(std::vector<uint8_t>& bytes,size_t at,uint32_t value) { std::memcpy(bytes.data()+at,&value,4); }
uint32_t Bits(float value) { uint32_t bits; std::memcpy(&bits,&value,4); return bits; }
struct Chunk { uint32_t tag; size_t offset,size; };
bool Chunks(const void* input,size_t size,std::vector<Chunk>& chunks) {
    if(!input || size<32 || size>MaxBytes) return false;
    auto* p=static_cast<const uint8_t*>(input);
    if(U(p)!=Tag('D','X','B','C') || U(p+20)!=1 || U(p+24)!=size) return false;
    const size_t count=U(p+28); if(!count || count>64 || count>(size-32)/4) return false;
    const size_t header=32+4*count;
    for(size_t i=0;i<count;++i) {
        size_t at=U(p+32+i*4);
        if(at<header || at>size-8 || (at&3)) return false;
        size_t length=U(p+at+4); if(length>size-at-8) return false;
        for(const auto& c:chunks) if(at<c.offset+8+c.size && c.offset<at+8+length) return false;
        chunks.push_back({U(p+at),at,length});
    }
    return true;
}
bool Decl(uint32_t op) {
    return (op>=D3D10_SB_OPCODE_DCL_RESOURCE && op<=D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS) ||
        (op>=D3D11_SB_OPCODE_DCL_STREAM && op<=D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED) || op==D3D11_SB_OPCODE_DCL_GS_INSTANCE_COUNT;
}
struct Instruction { size_t at,end,operands; uint32_t op; };
bool Instructions(const std::vector<uint32_t>& code,std::vector<Instruction>& instructions) {
    if(code.size()<2 || code[1]!=code.size()) return false;
    for(size_t at=2;at<code.size();) {
        uint32_t token=code[at],op=DECODE_D3D10_SB_OPCODE_TYPE(token);
        size_t count=DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(token);
        if(op==D3D10_SB_OPCODE_CUSTOMDATA) {
            if(at+1>=code.size()) return false;
            count=code[at+1]; if(count<2) return false;
        }
        if(!count || count>code.size()-at || op>=D3D10_SB_NUM_OPCODES) return false;
        size_t operands=at+1;
        if(op!=D3D10_SB_OPCODE_CUSTOMDATA) {
            while(token&0x80000000u) { if(operands>=at+count) return false; token=code[operands++]; }
        }
        instructions.push_back({at,at+count,operands,op}); at+=count;
    }
    return true;
}
struct Operand { size_t token,index; uint32_t type,dimensions,representation; };
// Parse every operand, including relative indices, before touching it. Immediate
// constants and custom-data payloads are never searched as if they were code.
bool ParseOperand(const std::vector<uint32_t>& code,size_t& at,size_t end,Operand* out=nullptr,unsigned depth=0) {
    if(at>=end || depth>16) return false;
    const size_t start=at; uint32_t token=code[at++],extended=token;
    while(extended&0x80000000u) { if(at>=end) return false; extended=code[at++]; }
    const uint32_t type=DECODE_D3D10_SB_OPERAND_TYPE(token),dims=DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(token);
    if(type>D3D11_SB_OPERAND_TYPE_INNER_COVERAGE) return false;
    const size_t first=at;
    for(uint32_t i=0;i<dims;++i) {
        uint32_t repr=DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(i,token);
        if(repr>4) return false;
        size_t immediate=repr==0 || repr==3 ? 1 : repr==1 || repr==4 ? 2 : 0;
        if(immediate>end-at) return false; at+=immediate;
        if(repr>=2 && !ParseOperand(code,at,end,nullptr,depth+1)) return false;
    }
    const uint32_t components=DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(token);
    if(components==3) return false; // variable-width operand format not supported
    if(type==D3D10_SB_OPERAND_TYPE_IMMEDIATE32 || type==D3D10_SB_OPERAND_TYPE_IMMEDIATE64) {
        if(dims || (components!=1 && components!=2)) return false;
        size_t words=(components==1?1u:4u)*(type==D3D10_SB_OPERAND_TYPE_IMMEDIATE64?2u:1u);
        if(words>end-at) return false; at+=words;
    }
    if(out) *out={start,first,type,dims,uint32_t(DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0,token))};
    return true;
}
uint32_t Destination(uint32_t type,uint32_t mask) { return 2u|(mask<<4)|(type<<12)|(1u<<20); }
uint32_t Source(uint32_t type,uint32_t swizzle) { return 2u|4u|(swizzle<<4)|(type<<12)|(1u<<20); }
uint32_t Scalar() { return 1u|(uint32_t(D3D10_SB_OPERAND_TYPE_IMMEDIATE32)<<12); }
void Emit(std::vector<uint32_t>& code,uint32_t op,std::initializer_list<uint32_t> operands,uint32_t flags=0) {
    code.push_back(op|ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(uint32_t(1+operands.size()))|flags);
    code.insert(code.end(),operands);
}
// Same stereo model and limits as the Vulkan SPIR-V rewrite (xgstereo/clip_patch.h):
// |w - 1| <= HudWTolerance is a HUD/full-screen pass, and the clip offset
// shift - convergence*w is clamped to +/- MaxScreenShift*|w| (NDC units).
constexpr float HudWTolerance=1.0e-4f,MaxScreenShift=.25f;
void Finish(std::vector<uint32_t>& code,uint32_t position,uint32_t saved,uint32_t scratch,Options options) {
    Emit(code,D3D10_SB_OPCODE_MOV,{Destination(D3D10_SB_OPERAND_TYPE_OUTPUT,15),position,Source(D3D10_SB_OPERAND_TYPE_TEMP,0xe4),saved});
    if(options.skipUnitW) {
        // scratch.x = w < 1-tol || w > 1+tol (outside the HUD tolerance)
        Emit(code,D3D10_SB_OPCODE_LT,{Destination(0,1),scratch,Source(0,0xff),saved,Scalar(),Bits(1.0f-HudWTolerance)});
        Emit(code,D3D10_SB_OPCODE_LT,{Destination(0,4),scratch,Scalar(),Bits(1.0f+HudWTolerance),Source(0,0xff),saved});
        Emit(code,D3D10_SB_OPCODE_OR,{Destination(0,1),scratch,Source(0,0),scratch,Source(0,0xaa),scratch});
        Emit(code,D3D10_SB_OPCODE_IF,{Source(0,0),scratch},ENCODE_D3D10_SB_INSTRUCTION_TEST_BOOLEAN(D3D10_SB_INSTRUCTION_TEST_NONZERO));
    }
    const float convergence=std::copysign(options.convergence,options.shift);
    // scratch.y = shift - convergence*w
    Emit(code,D3D10_SB_OPCODE_MAD,{Destination(0,2),scratch,Source(0,0xff),saved,Scalar(),Bits(-convergence),Scalar(),Bits(options.shift)});
    // scratch.z = MaxScreenShift*|w|; clamp scratch.y to [-z, z]
    Emit(code,D3D10_SB_OPCODE_MUL,{Destination(0,4),scratch,Source(0,0xff),saved,Scalar(),Bits(MaxScreenShift)});
    Emit(code,D3D10_SB_OPCODE_MUL,{Destination(0,8),scratch,Source(0,0xff),saved,Scalar(),Bits(-MaxScreenShift)});
    Emit(code,D3D10_SB_OPCODE_MAX,{Destination(0,4),scratch,Source(0,0xaa),scratch,Source(0,0xff),scratch});
    Emit(code,D3D10_SB_OPCODE_MIN,{Destination(0,2),scratch,Source(0,0x55),scratch,Source(0,0xaa),scratch});
    Emit(code,D3D10_SB_OPCODE_MUL,{Destination(0,8),scratch,Source(0,0xaa),scratch,Scalar(),Bits(-1.0f)});
    Emit(code,D3D10_SB_OPCODE_MAX,{Destination(0,2),scratch,Source(0,0x55),scratch,Source(0,0xff),scratch});
    Emit(code,D3D10_SB_OPCODE_ADD,{Destination(0,2),scratch,Source(0,0x55),scratch,Source(0,0),saved});
    Emit(code,D3D10_SB_OPCODE_MOV,{Destination(D3D10_SB_OPERAND_TYPE_OUTPUT,1),position,Source(0,0x55),scratch});
    if(options.skipUnitW) Emit(code,D3D10_SB_OPCODE_ENDIF,{});
}
Result Fail(Status status,const char* reason) { return {status,{},reason}; }
} // namespace
bool ValidChecksum(const void* data,size_t size) {
    std::vector<Chunk> chunks; if(!Chunks(data,size,chunks)) return false;
    uint8_t hash[16]{}; auto* p=static_cast<const uint8_t*>(data);
    ComputeHashRetail(p+20,UINT(size-20),hash); return std::memcmp(p+4,hash,16)==0;
}
Result Rewrite(const void* input,size_t size,Options options) {
    if(!std::isfinite(options.shift) || std::abs(options.shift)>64 || !std::isfinite(options.convergence) || options.convergence<0 || options.convergence>.25f)
        return Fail(Status::Invalid,"invalid stereo calibration");
    std::vector<Chunk> chunks;
    if(!Chunks(input,size,chunks)) return Fail(Status::Invalid,"invalid DXBC container");
    auto* p=static_cast<const uint8_t*>(input); const Chunk* shader=nullptr;
    for(const auto& chunk:chunks) {
        if(chunk.tag==Tag('D','X','I','L')) return Fail(Status::Unsupported,"DXIL needs a separate transform");
        if(chunk.tag==Tag('S','H','E','X') || chunk.tag==Tag('S','H','D','R')) {
            if(shader) return Fail(Status::Invalid,"duplicate shader chunks"); shader=&chunk;
        }
    }
    if(!ValidChecksum(input,size)) return Fail(Status::Invalid,"invalid DXBC checksum");
    if(!shader || shader->size<8 || (shader->size&3)) return Fail(Status::Invalid,"missing or invalid shader program");
    std::vector<uint32_t> code(shader->size/4); std::memcpy(code.data(),p+shader->offset+8,shader->size);
    const auto stage=DECODE_D3D10_SB_TOKENIZED_PROGRAM_TYPE(code[0]);const bool geometry=stage==D3D10_SB_GEOMETRY_SHADER;
    if((stage!=D3D10_SB_VERTEX_SHADER && !geometry) ||
        DECODE_D3D10_SB_TOKENIZED_PROGRAM_MAJOR_VERSION(code[0])!=5 || DECODE_D3D10_SB_TOKENIZED_PROGRAM_MINOR_VERSION(code[0])>1)
        return Fail(Status::Unsupported,"only shader-model-5 vertex/geometry programs are supported");
    std::vector<Instruction> instructions; if(!Instructions(code,instructions)) return Fail(Status::Invalid,"invalid instruction bounds");
    uint32_t position=UINT32_MAX,temps=0; bool hasTemps=false;
    for(const auto& instruction:instructions) {
        const auto op=instruction.op;
        if(op==D3D10_SB_OPCODE_CALL || op==D3D10_SB_OPCODE_CALLC || op==D3D10_SB_OPCODE_LABEL || op==D3D11_SB_OPCODE_INTERFACE_CALL ||
            op==D3D11_SB_OPCODE_DCL_FUNCTION_BODY || op==D3D11_SB_OPCODE_DCL_FUNCTION_TABLE || op==D3D11_SB_OPCODE_DCL_INTERFACE)
            return Fail(Status::Unsupported,"shader linkage and subroutines need a separate transform");
        if(op==D3D10_SB_OPCODE_DCL_TEMPS) {
            if(hasTemps || instruction.end-instruction.operands!=1) return Fail(Status::Invalid,"invalid temporary declaration");
            temps=code[instruction.operands]; hasTemps=true;
        }
        if(geometry && op==D3D11_SB_OPCODE_DCL_STREAM) {
            size_t at=instruction.operands;Operand stream{};
            if(!ParseOperand(code,at,instruction.end,&stream) || at!=instruction.end || stream.type!=D3D11_SB_OPERAND_TYPE_STREAM || stream.dimensions!=1 || stream.representation!=0 || code[stream.index]!=0)
                return Fail(Status::Unsupported,"geometry output streams other than stream zero require separate handling");
        }
        if(op==D3D10_SB_OPCODE_DCL_OUTPUT_SIV) {
            size_t at=instruction.operands; Operand output{};
            if(!ParseOperand(code,at,instruction.end,&output) || at+1!=instruction.end) return Fail(Status::Invalid,"invalid position declaration");
            if(code[at]==D3D10_SB_NAME_POSITION) {
                if(position!=UINT32_MAX || output.type!=D3D10_SB_OPERAND_TYPE_OUTPUT || output.dimensions!=1 || output.representation!=0 ||
                    (code[output.token]&0xfff)!=0xf2) return Fail(Status::Unsupported,"unsupported position register layout");
                position=code[output.index];
            }
        }
    }
    if(position==UINT32_MAX || temps>4094) return Fail(Status::Unsupported,"position or spare temporary registers unavailable");
    std::vector<uint32_t> rewritten{code[0],0}; bool inserted=hasTemps; uint32_t returns=0;
    for(const auto& instruction:instructions) {
        if(instruction.end-instruction.at+64>MaxBytes/4-rewritten.size()) return Fail(Status::Unsupported,"rewritten program exceeds the size bound");
        if(!inserted && !Decl(instruction.op) && instruction.op!=D3D10_SB_OPCODE_CUSTOMDATA) {
            Emit(rewritten,D3D10_SB_OPCODE_DCL_TEMPS,{temps+2}); inserted=true;
        }
        const auto op=instruction.op;
        const bool emit=op==D3D10_SB_OPCODE_EMIT || op==D3D10_SB_OPCODE_EMITTHENCUT || op==D3D11_SB_OPCODE_EMIT_STREAM || op==D3D11_SB_OPCODE_EMITTHENCUT_STREAM;
        if(geometry?emit:op==D3D10_SB_OPCODE_RET || op==D3D10_SB_OPCODE_RETC) { Finish(rewritten,position,temps,temps+1,options); ++returns; }
        const size_t base=rewritten.size(); rewritten.insert(rewritten.end(),code.begin()+instruction.at,code.begin()+instruction.end);
        if(instruction.op==D3D10_SB_OPCODE_DCL_TEMPS) rewritten[base+instruction.operands-instruction.at]=temps+2;
        if(!Decl(instruction.op) && instruction.op!=D3D10_SB_OPCODE_CUSTOMDATA) {
            size_t at=instruction.operands;
            while(at<instruction.end) {
                Operand operand{};
                if(!ParseOperand(code,at,instruction.end,&operand)) return Fail(Status::Unsupported,"unsupported operand encoding");
                if(operand.type==D3D10_SB_OPERAND_TYPE_OUTPUT) {
                    if(operand.dimensions!=1 || operand.representation!=0) return Fail(Status::Unsupported,"relative output registers unsupported");
                    if(code[operand.index]==position) {
                        rewritten[base+operand.token-instruction.at]&=~uint32_t(D3D10_SB_OPERAND_TYPE_MASK);
                        rewritten[base+operand.index-instruction.at]=temps;
                    }
                }
            }
        }
    }
    if(!returns || !inserted) return Fail(Status::Invalid,geometry?"geometry program has no supported emission":"vertex program has no return");
    rewritten[1]=uint32_t(rewritten.size());
    std::vector<Chunk> kept;
    for(const auto& chunk:chunks) if(chunk.tag!=Tag('S','D','B','G') && chunk.tag!=Tag('S','P','D','B') && chunk.tag!=Tag('I','L','D','B') &&
        chunk.tag!=Tag('S','T','A','T') && chunk.tag!=Tag('H','A','S','H')) kept.push_back(chunk);
    Result result{Status::Patched,std::vector<uint8_t>(32+kept.size()*4),{}};
    std::memcpy(result.bytes.data(),p,24); W(result.bytes,28,uint32_t(kept.size()));
    for(size_t i=0;i<kept.size();++i) {
        const auto& chunk=kept[i]; while(result.bytes.size()&3) result.bytes.push_back(0);
        size_t at=result.bytes.size(); W(result.bytes,32+i*4,uint32_t(at));
        const bool replace=chunk.offset==shader->offset;
        const size_t bytes=replace?rewritten.size()*4:chunk.size;
        if(at>MaxBytes-8 || bytes>MaxBytes-at-8) return Fail(Status::Unsupported,"rewritten container exceeds the size bound");
        result.bytes.resize(at+8+bytes); W(result.bytes,at,chunk.tag); W(result.bytes,at+4,uint32_t(bytes));
        std::memcpy(result.bytes.data()+at+8,replace?reinterpret_cast<const uint8_t*>(rewritten.data()):p+chunk.offset+8,bytes);
    }
    W(result.bytes,24,uint32_t(result.bytes.size()));
    ComputeHashRetail(result.bytes.data()+20,UINT(result.bytes.size()-20),result.bytes.data()+4);
    return result;
}
} // namespace vrx::dx12::dxbc
