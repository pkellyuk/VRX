#include "spirv_push_addresses.h"
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <initializer_list>
using Words=std::vector<uint32_t>;
static void Emit(Words& w,spv::Op op,std::initializer_list<uint32_t> args){w.push_back((uint32_t(args.size()+1)<<16)|op);w.insert(w.end(),args.begin(),args.end());}
static void Check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
static Words Base(){
    Words w{spv::MagicNumber,0x00010300,0,100,0};
    Emit(w,spv::OpTypeInt,{1,32,0});Emit(w,spv::OpTypeInt,{2,64,0});
    Emit(w,spv::OpTypeStruct,{3,2,1});Emit(w,spv::OpTypePointer,{4,spv::StorageClassPushConstant,3});
    Emit(w,spv::OpTypePointer,{5,spv::StorageClassPushConstant,2});Emit(w,spv::OpTypePointer,{6,spv::StorageClassPhysicalStorageBuffer,1});
    Emit(w,spv::OpMemberDecorate,{3,0,spv::DecorationOffset,16});Emit(w,spv::OpMemberDecorate,{3,1,spv::DecorationOffset,24});
    Emit(w,spv::OpConstant,{1,7,0});Emit(w,spv::OpConstant,{2,8,4,0});
    Emit(w,spv::OpVariable,{4,9,spv::StorageClassPushConstant});Emit(w,spv::OpAccessChain,{5,10,9,7});
    Emit(w,spv::OpLoad,{2,11,10});return w;
}
int main(int argc,char** argv){try{
    if(argc>1){
        std::ifstream file(argv[1],std::ios::binary|std::ios::ate);Check(bool(file),"open SPIR-V file");const auto bytes=file.tellg();Check(bytes>0 && size_t(bytes)%4==0,"SPIR-V byte count");
        Words words(size_t(bytes)/4);file.seekg(0);file.read(reinterpret_cast<char*>(words.data()),bytes);Check(bool(file),"read SPIR-V file");
        const auto r=vrx::spirv::IntegerPushAddresses(words.data(),words.size());std::printf("supported=%d offsets=",int(r.supported));for(auto offset:r.offsets)std::printf("%u,",offset);std::puts("");return r.supported?0:2;
    }
    auto w=Base();Emit(w,spv::OpIAdd,{2,12,11,8});Emit(w,spv::OpConvertUToPtr,{6,13,12});
    const auto r=vrx::spirv::IntegerPushAddresses(w.data(),w.size());Check(r.supported && r.offsets==std::vector<uint32_t>{16},"base plus constant offset");
    auto local=Base();Emit(local,spv::OpTypePointer,{20,spv::StorageClassFunction,2});Emit(local,spv::OpVariable,{20,21,spv::StorageClassFunction});Emit(local,spv::OpStore,{21,11});Emit(local,spv::OpLoad,{2,22,21});Emit(local,spv::OpConvertUToPtr,{6,23,22});
    Check(vrx::spirv::IntegerPushAddresses(local.data(),local.size()).offsets==std::vector<uint32_t>{16},"single store local provenance");
    auto unknown=Base();Emit(unknown,spv::OpLoad,{2,12,80});Emit(unknown,spv::OpConvertUToPtr,{6,13,12});Check(!vrx::spirv::IntegerPushAddresses(unknown.data(),unknown.size()).supported,"GPU-loaded 64-bit address rejected");
    auto truncated=Base();Emit(truncated,spv::OpUConvert,{1,12,11});Emit(truncated,spv::OpUConvert,{2,13,12});Emit(truncated,spv::OpConvertUToPtr,{6,14,13});Check(!vrx::spirv::IntegerPushAddresses(truncated.data(),truncated.size()).supported,"truncated address rejected");
    auto doubled=Base();Emit(doubled,spv::OpIAdd,{2,12,11,11});Emit(doubled,spv::OpConvertUToPtr,{6,13,12});Check(!vrx::spirv::IntegerPushAddresses(doubled.data(),doubled.size()).supported,"sum of addresses rejected");
    auto numeric=Base();Emit(numeric,spv::OpConvertUToPtr,{6,13,8});Check(!vrx::spirv::IntegerPushAddresses(numeric.data(),numeric.size()).supported,"nonzero constant address rejected");
    auto bitcast=Base();Emit(bitcast,spv::OpBitcast,{6,13,11});Check(vrx::spirv::IntegerPushAddresses(bitcast.data(),bitcast.size()).offsets==std::vector<uint32_t>{16},"integer bitcast pointer provenance");
    auto pointerLocal=Base();Emit(pointerLocal,spv::OpConvertUToPtr,{6,13,11});Emit(pointerLocal,spv::OpTypePointer,{20,spv::StorageClassFunction,6});Emit(pointerLocal,spv::OpVariable,{20,21,spv::StorageClassFunction});Emit(pointerLocal,spv::OpStore,{21,13});Emit(pointerLocal,spv::OpLoad,{6,22,21});Check(vrx::spirv::IntegerPushAddresses(pointerLocal.data(),pointerLocal.size()).supported,"converted pointer saved in a function local");
    auto nested=Base();Emit(nested,spv::OpLoad,{6,13,80});Check(!vrx::spirv::IntegerPushAddresses(nested.data(),nested.size()).supported,"GPU-loaded physical pointer rejected");
    auto null=Base();Emit(null,spv::OpConstantNull,{2,12});Emit(null,spv::OpConvertUToPtr,{6,13,12});Check(vrx::spirv::IntegerPushAddresses(null.data(),null.size()).supported,"null address accepted");
    auto malformed=w;malformed.push_back(0);Check(!vrx::spirv::IntegerPushAddresses(malformed.data(),malformed.size()).supported,"malformed instruction rejected");
    Check(!vrx::spirv::IntegerPushAddresses(nullptr,0).supported,"empty shader rejected");
    std::puts("PASS: bounded push-address provenance and rejection of unrelocatable pointers");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
