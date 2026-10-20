// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Prove which eight-byte push fields feed integer-to-physical-pointer casts.
// Unknown GPU-loaded addresses are rejected; ordinary integer data is not scanned.
#pragma once
#include <spirv/unified1/spirv.hpp>
#include <cstdint>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <algorithm>
namespace vrx::spirv {
struct PushAddresses { bool supported = true; std::vector<uint32_t> offsets; };
inline PushAddresses IntegerPushAddresses(const uint32_t* words, size_t count) {
    PushAddresses result;
    if (!words || count < 5 || words[0] != spv::MagicNumber || count > 1048576) { result.supported=false; return result; }
    struct Def { uint32_t op=0, type=0; std::vector<uint32_t> args; };
    struct Type { uint32_t op=0, a=0, b=0; std::vector<uint32_t> members; };
    std::map<uint32_t,Def> defs;
    std::map<uint32_t,Type> types;
    std::map<uint32_t,uint32_t> constants, strides;
    std::map<std::pair<uint32_t,uint32_t>,uint32_t> members;
    std::map<uint32_t,std::vector<uint32_t>> stores;
    std::vector<uint32_t> casts;
    std::vector<uint32_t> pointerLoads;
    for (size_t at=5;at<count;) {
        const auto* w=words+at;const uint32_t n=w[0]>>16, op=w[0]&65535;
        if (!n || n>count-at) {result.supported=false;return result;}
        if (op==spv::OpTypeInt && n==4) types[w[1]]={op,w[2],w[3],{}};
        if ((op==spv::OpTypePointer || op==spv::OpTypeVector || op==spv::OpTypeArray) && n==4) types[w[1]]={op,w[2],w[3],{}};
        if (op==spv::OpTypeStruct && n>=2) {Type t;t.op=op;t.members.assign(w+2,w+n);types[w[1]]=std::move(t);}
        if (op==spv::OpMemberDecorate && n==5 && w[3]==spv::DecorationOffset) members[{w[1],w[2]}]=w[4];
        if (op==spv::OpDecorate && n==4 && w[2]==spv::DecorationArrayStride) strides[w[1]]=w[3];
        if (op==spv::OpConstant && n==4) constants[w[2]]=w[3];
        if (op==spv::OpStore && n>=3) stores[w[1]].push_back(w[2]);
        bool typed=false;
        switch(op) {
        case spv::OpConstant: case spv::OpConstantNull: case spv::OpVariable: case spv::OpLoad:
        case spv::OpAccessChain: case spv::OpInBoundsAccessChain: case spv::OpCopyObject:
        case spv::OpBitcast: case spv::OpUConvert: case spv::OpSConvert:
        case spv::OpIAdd: case spv::OpISub: case spv::OpIMul: case spv::OpShiftLeftLogical:
        case spv::OpSelect: case spv::OpPhi: case spv::OpConvertUToPtr:
            typed=true;break;
        default: break;
        }
        if (typed && n>=3) {Def d;d.op=op;d.type=w[1];d.args.assign(w+3,w+n);defs[w[2]]=std::move(d);}
        if (op==spv::OpConvertUToPtr && n==4) casts.push_back(w[3]);
        if ((op==spv::OpBitcast || op==spv::OpLoad) && n>=4) {
            const auto t=types.find(w[1]);
            if(t!=types.end() && t->second.op==spv::OpTypePointer && t->second.a==spv::StorageClassPhysicalStorageBuffer) {
                if(op==spv::OpBitcast)casts.push_back(w[3]);else pointerLoads.push_back(w[2]);
            }
        }
        at+=n;
    }
    std::function<uint32_t(uint32_t)> bits=[&](uint32_t id)->uint32_t {
        const auto it=types.find(id);if(it==types.end())return 0;
        if(it->second.op==spv::OpTypeInt)return it->second.a;
        if(it->second.op==spv::OpTypeVector && it->second.b<=4) {
            const auto el=types.find(it->second.a);
            if(el!=types.end() && el->second.op==spv::OpTypeInt)return el->second.a*it->second.b;
        }
        return 0;
    };
    struct Location {bool valid=false;uint32_t type=0,offset=0;};
    std::function<Location(uint32_t,uint32_t)> location=[&](uint32_t id,uint32_t depth)->Location {
        if(depth>48)return {};
        const auto it=defs.find(id);if(it==defs.end())return {};
        const auto& d=it->second;
        if(d.op==spv::OpVariable && !d.args.empty() && d.args[0]==spv::StorageClassPushConstant) {
            const auto t=types.find(d.type);
            if(t!=types.end() && t->second.op==spv::OpTypePointer)return {true,t->second.b,0};
        }
        if(d.op==spv::OpCopyObject && d.args.size()==1)return location(d.args[0],depth+1);
        if((d.op!=spv::OpAccessChain && d.op!=spv::OpInBoundsAccessChain)||d.args.empty())return {};
        auto l=location(d.args[0],depth+1);if(!l.valid)return {};
        for(size_t i=1;i<d.args.size();++i) {
            const auto c=constants.find(d.args[i]);const auto t=types.find(l.type);
            if(c==constants.end() || t==types.end())return {};
            const auto index=c->second;const auto& ty=t->second;
            uint64_t offset=l.offset;
            if(ty.op==spv::OpTypeStruct) {
                if(index>=ty.members.size())return {};
                const auto m=members.find({l.type,index});if(m==members.end())return {};
                offset+=m->second;l.type=ty.members[index];
            } else if(ty.op==spv::OpTypeArray) {
                const auto length=constants.find(ty.b),stride=strides.find(l.type);
                if(length==constants.end()||stride==strides.end()||index>=length->second)return {};
                offset+=uint64_t(index)*stride->second;l.type=ty.a;
            } else return {};
            if(offset>4096)return {};l.offset=uint32_t(offset);
        }
        return l;
    };
    struct Origin {bool valid=false,zero=false;std::set<uint32_t> fields;};
    const auto numeric=[](bool zero=false){return Origin{true,zero,{}};};
    std::set<uint32_t> visiting;
    std::map<uint32_t,Origin> memo;
    std::function<Origin(uint32_t,uint32_t)> origin;
    origin=[&](uint32_t id,uint32_t depth)->Origin {
        if(depth>64)return {};
        const auto cached=memo.find(id);if(cached!=memo.end())return cached->second;
        if(!visiting.insert(id).second)return {};
        const auto evaluate=[&]()->Origin {
            const auto it=defs.find(id);if(it==defs.end())return {};
            const auto& d=it->second;const auto width=bits(d.type);
            if(d.op==spv::OpConstantNull)return numeric(true);
            if(d.op==spv::OpConstant) {
                bool zero=true;for(auto value:d.args)zero=zero && value==0;return numeric(zero);
            }
            if(d.op==spv::OpLoad && !d.args.empty()) {
                const auto l=location(d.args[0],0);
                if(l.valid && width==64 && bits(l.type)==64)return {true,false,{l.offset}};
                const auto loaded=types.find(d.type);
                if(l.valid && loaded!=types.end() && loaded->second.op==spv::OpTypePointer && loaded->second.a==spv::StorageClassPhysicalStorageBuffer)return {true,false,{l.offset}};
                const auto local=defs.find(d.args[0]);
                if(local!=defs.end() && local->second.op==spv::OpVariable && !local->second.args.empty() && local->second.args[0]==spv::StorageClassFunction) {
                    const auto s=stores.find(d.args[0]);
                    if(s!=stores.end() && s->second.size()==1)return origin(s->second[0],depth+1);
                }
                // A 32-bit scalar can be a byte offset. A GPU-loaded 64-bit
                // quantity may be an address and cannot be relocated on the CPU.
                if(width && width<=32)return numeric();
                return {};
            }
            if((d.op==spv::OpCopyObject || d.op==spv::OpBitcast || d.op==spv::OpUConvert || d.op==spv::OpSConvert || d.op==spv::OpConvertUToPtr) && d.args.size()==1) {
                auto o=origin(d.args[0],depth+1);
                const auto source=defs.find(d.args[0]);
                const auto destination=types.find(d.type);
                const bool physical=destination!=types.end() && destination->second.op==spv::OpTypePointer && destination->second.a==spv::StorageClassPhysicalStorageBuffer;
                if(!o.fields.empty() && !physical && (width!=64 || source==defs.end() || bits(source->second.type)!=64))return {};
                return o;
            }
            if((d.op==spv::OpAccessChain || d.op==spv::OpInBoundsAccessChain) && !d.args.empty()) {
                const auto t=types.find(d.type);
                if(t!=types.end() && t->second.op==spv::OpTypePointer && t->second.a==spv::StorageClassPhysicalStorageBuffer)return origin(d.args[0],depth+1);
            }
            if((d.op==spv::OpIAdd || d.op==spv::OpISub || d.op==spv::OpIMul || d.op==spv::OpShiftLeftLogical) && d.args.size()==2) {
                auto a=origin(d.args[0],depth+1),b=origin(d.args[1],depth+1);
                if(!a.valid || !b.valid)return {};
                if(d.op==spv::OpIAdd) {
                    if(!a.fields.empty() && !b.fields.empty())return {};
                    a.fields.insert(b.fields.begin(),b.fields.end());a.zero=a.zero&&b.zero;return a;
                }
                if(d.op==spv::OpISub && b.fields.empty()){a.zero=a.zero&&b.zero;return a;}
                if(a.fields.empty() && b.fields.empty())return numeric();
                return {};
            }
            // Only identical origins can merge. Rewriting two distinct push
            // fields chosen by a conditional could change an unrelated integer.
            if(d.op==spv::OpSelect && d.args.size()==3) {
                auto a=origin(d.args[1],depth+1),b=origin(d.args[2],depth+1);
                if(a.valid && b.valid && a.fields==b.fields){a.zero=a.zero&&b.zero;return a;}
            }
            if(width && width<=32)return numeric();
            return {};
        };
        auto o=evaluate();visiting.erase(id);memo[id]=o;return o;
    };
    casts.insert(casts.end(),pointerLoads.begin(),pointerLoads.end());
    for(auto id:casts) {
        const auto o=origin(id,0);
        if(!o.valid || (o.fields.empty() && !o.zero)){result.supported=false;continue;}
        result.offsets.insert(result.offsets.end(),o.fields.begin(),o.fields.end());
    }
    std::sort(result.offsets.begin(),result.offsets.end());
    result.offsets.erase(std::unique(result.offsets.begin(),result.offsets.end()),result.offsets.end());
    return result;
}
}
