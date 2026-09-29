#pragma once
// GPU depth image operations. All image-sized data stays in DEFAULT resources;
// ReadSummary reads only the 49x28 tracker grid and a few scalar diagnostics.
// CPU depth_fusion.h remains the executable numerical reference.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

namespace gpudepth {
using Microsoft::WRL::ComPtr;
inline void Check(HRESULT hr, const char *what) {
  if (FAILED(hr))
    throw std::runtime_error(std::string("GPU depth: ") + what);
}
// Used only on recovery: fence all commands already submitted to an externally
// owned inference/preparation queue before its inputs can be reused.
inline void RetireQueue(ID3D12Device *device, ID3D12CommandQueue *queue) {
  if (!queue)
    return;
  ComPtr<ID3D12Fence> fence;
  Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
        "recovery fence");
  Check(queue->Signal(fence.Get(), 1), "recovery signal");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event)
    throw std::runtime_error("GPU depth: recovery event");
  const HRESULT hr = fence->SetEventOnCompletion(1, event);
  const DWORD result =
      SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
  CloseHandle(event);
  Check(hr, "recovery fence event");
  if (result != WAIT_OBJECT_0 || fence->GetCompletedValue() == UINT64_MAX)
    throw std::runtime_error("GPU depth: recovery queue did not retire");
}
inline void Transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
                       D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
  if (a == b)
    return;
  D3D12_RESOURCE_BARRIER t{};
  t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  t.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
  cl->ResourceBarrier(1, &t);
}
inline ComPtr<ID3D12Resource>
Buffer(ID3D12Device *dev, UINT64 bytes,
       D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT) {
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = heap;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = (bytes + 3) & ~UINT64(3);
  rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd.Flags = heap == D3D12_HEAP_TYPE_DEFAULT
                 ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                 : D3D12_RESOURCE_FLAG_NONE;
  ComPtr<ID3D12Resource> out;
  Check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                     heap == D3D12_HEAP_TYPE_DEFAULT
                                         ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                     : heap == D3D12_HEAP_TYPE_UPLOAD
                                         ? D3D12_RESOURCE_STATE_GENERIC_READ
                                         : D3D12_RESOURCE_STATE_COPY_DEST,
                                     nullptr, IID_PPV_ARGS(&out)),
        "buffer allocation");
  return out;
}

class Queue {
public:
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  UINT64 value = 0;
  HANDLE event = nullptr;
  Queue() = default;
  Queue(const Queue &) = delete;
  ~Queue() {
    if (fence && event) {
      try {
        Wait();
      } catch (...) {
      }
    }
    if (event)
      CloseHandle(event);
  }
  void Init(ID3D12Device *d, const wchar_t *name = L"VRX GPU depth") {
    device = d;
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(d->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
    queue->SetName(name);
    Check(d->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)),
          "allocator");
    Check(d->CreateCommandList(0, q.Type, allocator.Get(), nullptr,
                               IID_PPV_ARGS(&list)),
          "command list");
    Check(list->Close(), "close");
    Check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
          "fence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
      throw std::runtime_error("GPU depth: fence event");
  }
  void Wait() {
    if (!value)
      return;
    const UINT64 completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX)
      throw std::runtime_error("GPU depth: device removed");
    if (completed >= value)
      return;
    Check(fence->SetEventOnCompletion(value, event), "fence event");
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0 ||
        fence->GetCompletedValue() == UINT64_MAX)
      throw std::runtime_error("GPU depth: GPU timeout/device lost");
  }
  void Begin() {
    Wait();
    Check(allocator->Reset(), "allocator reset");
    Check(list->Reset(allocator.Get(), nullptr), "list reset");
  }
  UINT64 Submit() {
    Check(list->Close(), "list close");
    ID3D12CommandList *l[] = {list.Get()};
    queue->ExecuteCommandLists(1, l);
    Check(queue->Signal(fence.Get(), ++value), "signal");
    return value;
  }
  void After(ID3D12Fence *f, UINT64 v) {
    if (f && v)
      Check(queue->Wait(f, v), "queue dependency");
  }
  void
  Copy(ID3D12Resource *to, ID3D12Resource *from, UINT64 bytes,
       D3D12_RESOURCE_STATES fromState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
       D3D12_RESOURCE_STATES toState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    Transition(list.Get(), from, fromState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list.Get(), to, toState, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(to, 0, from, 0, bytes);
    Transition(list.Get(), from, D3D12_RESOURCE_STATE_COPY_SOURCE, fromState);
    Transition(list.Get(), to, D3D12_RESOURCE_STATE_COPY_DEST, toState);
  }
};

// A separate, bidirectional-capable GPU-only bridge. Transfer waits on its own
// queues, never copies pixels through a CPU mapping. Resources return to UAV.
class Bridge {
  Queue send_, receive_;
  ComPtr<ID3D12Heap> heapA_, heapB_;
  ComPtr<ID3D12Resource> a_, b_;
  ComPtr<ID3D12Fence> sharedA_, sharedB_;
  UINT64 sequence_ = 0, bytes_ = 0;
  bool same_ = false;

public:
  ~Bridge() {
    // Drain while heaps and the resource objects used by submitted lists live.
    try {
      Wait();
    } catch (...) {
    }
  }
  void Wait() {
    if (send_.fence)
      send_.Wait();
    if (receive_.fence)
      receive_.Wait();
  }
  void Init(ID3D12Device *from, ID3D12Device *to, UINT64 bytes) {
    bytes_ = bytes;
    same_ = from == to;
    send_.Init(from, L"VRX depth transfer send");
    if (same_)
      return;
    receive_.Init(to, L"VRX depth transfer receive");
    D3D12_RESOURCE_DESC r{};
    r.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    r.Width = (bytes + 65535) & ~UINT64(65535);
    r.Height = r.DepthOrArraySize = r.MipLevels = 1;
    r.SampleDesc.Count = 1;
    r.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    r.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    D3D12_HEAP_DESC h{};
    h.SizeInBytes = r.Width;
    h.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    h.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    Check(from->CreateHeap(&h, IID_PPV_ARGS(&heapA_)), "cross adapter heap");
    HANDLE handle = nullptr;
    Check(from->CreateSharedHandle(heapA_.Get(), nullptr, GENERIC_ALL, nullptr,
                                   &handle),
          "shared heap handle");
    HRESULT hr = to->OpenSharedHandle(handle, IID_PPV_ARGS(&heapB_));
    CloseHandle(handle);
    Check(hr, "open shared heap");
    Check(from->CreatePlacedResource(heapA_.Get(), 0, &r,
                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                     IID_PPV_ARGS(&a_)),
          "shared source");
    Check(to->CreatePlacedResource(heapB_.Get(), 0, &r,
                                   D3D12_RESOURCE_STATE_COMMON, nullptr,
                                   IID_PPV_ARGS(&b_)),
          "shared destination");
    Check(from->CreateFence(0,
                            D3D12_FENCE_FLAG_SHARED |
                                D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
                            IID_PPV_ARGS(&sharedA_)),
          "shared fence");
    Check(from->CreateSharedHandle(sharedA_.Get(), nullptr, GENERIC_ALL,
                                   nullptr, &handle),
          "fence handle");
    hr = to->OpenSharedHandle(handle, IID_PPV_ARGS(&sharedB_));
    CloseHandle(handle);
    Check(hr, "open shared fence");
  }
  void Transfer(ID3D12Resource *src, ID3D12Resource *dst, UINT64 bytes,
                ID3D12Fence *ready = nullptr, UINT64 readyValue = 0) {
    if (bytes > bytes_)
      throw std::runtime_error("GPU depth: bridge size");
    if (!same_)
      receive_.Wait();
    send_.Begin();
    send_.After(ready, readyValue);
    if (same_) {
      send_.Copy(dst, src, bytes);
      send_.Submit();
      send_.Wait();
      return;
    }
    send_.Copy(a_.Get(), src, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COMMON);
    send_.Submit();
    Check(send_.queue->Signal(sharedA_.Get(), ++sequence_), "bridge signal");
    receive_.Begin();
    receive_.After(sharedB_.Get(), sequence_);
    receive_.Copy(dst, b_.Get(), bytes, D3D12_RESOURCE_STATE_COMMON);
    receive_.Submit();
    receive_.Wait();
  }
};

inline constexpr const char *Shader = R"HLSL(
cbuffer C:register(b0){uint w,h,sw,sh;uint mode,radius,axis,flags;float p0,p1,p2,p3;float p4,p5,p6,p7;uint pitch,bw,bh,count;}
RWByteAddressBuffer A:register(u0);RWByteAddressBuffer B:register(u1);RWByteAddressBuffer D:register(u2);RWByteAddressBuffer E:register(u3);
RWByteAddressBuffer O:register(u4);RWByteAddressBuffer P:register(u5);RWByteAddressBuffer Q:register(u6);RWByteAddressBuffer R:register(u7);
float loadA(uint i){return asfloat(A.Load(i*4));}float loadB(uint i){return asfloat(B.Load(i*4));}
float sampleA(float2 p,uint2 size){p=clamp(p,0,float2(size)-1);uint2 a=(uint2)p,b=min(a+1,size-1);float2 t=p-a;
return lerp(lerp(loadA(a.y*size.x+a.x),loadA(a.y*size.x+b.x),t.x),lerp(loadA(b.y*size.x+a.x),loadA(b.y*size.x+b.x),t.x),t.y);}
float sampleB(float2 p,uint2 size){p=clamp(p,0,float2(size)-1);uint2 a=(uint2)p,b=min(a+1,size-1);float2 t=p-a;
return lerp(lerp(loadB(a.y*size.x+a.x),loadB(a.y*size.x+b.x),t.x),lerp(loadB(b.y*size.x+a.x),loadB(b.y*size.x+b.x),t.x),t.y);}
int reflect(int x,int n){while(x<0||x>=n){if(x<0)x=-x-1;if(x>=n)x=2*n-x-1;}return x;}
[numthreads(16,16,1)]void Resample(uint3 t:SV_DispatchThreadID){if(t.x>=w||t.y>=h)return;O.Store((t.y*w+t.x)*4,asuint(sampleA((t.xy+.5)*float2(sw,sh)/float2(w,h)-.5,uint2(sw,sh))));}
groupshared float mins[256];groupshared float maxs[256];groupshared uint bads[256];
[numthreads(256,1,1)]void RangeReduce(uint3 t:SV_DispatchThreadID,uint gi:SV_GroupIndex,uint3 g:SV_GroupID){uint i=t.x;float v=i<w*h?loadA(i):0;bool good=isfinite(v);
mins[gi]=i<w*h&&good?v:3.402823e38;maxs[gi]=i<w*h&&good?v:-3.402823e38;bads[gi]=i<w*h&&!good?1:0;GroupMemoryBarrierWithGroupSync();
for(uint s=128;s>0;s>>=1){if(gi<s){mins[gi]=min(mins[gi],mins[gi+s]);maxs[gi]=max(maxs[gi],maxs[gi+s]);bads[gi]+=bads[gi+s];}GroupMemoryBarrierWithGroupSync();}
if(gi==0){O.Store(g.x*16,asuint(mins[0]));O.Store(g.x*16+4,asuint(maxs[0]));O.Store(g.x*16+8,bads[0]);}}
[numthreads(1,1,1)]void RangeFinish(uint3 t:SV_DispatchThreadID){float lo=3.402823e38,hi=-3.402823e38;uint bad=0;
for(uint i=0;i<count;i++){lo=min(lo,asfloat(A.Load(i*16)));hi=max(hi,asfloat(A.Load(i*16+4)));bad+=A.Load(i*16+8);}O.Store(0,asuint(lo));O.Store(4,asuint(hi));O.Store(8,bad);}
[numthreads(256,1,1)]void Clear(uint3 t:SV_DispatchThreadID){if(t.x<count)O.Store(t.x*4,0);}
[numthreads(256,1,1)]void Histogram(uint3 t:SV_DispatchThreadID){if(t.x>=w*h)return;float lo=mode==1?0:loadB(0),hi=mode==1?1:loadB(1),v=loadA(t.x);
uint bin=hi-lo>1e-6&&isfinite(v)?(uint)clamp((v-lo)*1023/(hi-lo),0,1023):0;O.InterlockedAdd(bin*4,1);}
[numthreads(1,1,1)]void NormalizeRange(uint3 t:SV_DispatchThreadID){float lo=loadB(0),hi=loadB(1),nlo=lo,nhi=hi;uint low=(uint)(w*h*.005),high=(uint)(w*h*.995),acc=0;bool found=false;
if(flags!=0&&hi-lo>=1e-6){for(uint i=0;i<1024;i++){acc+=A.Load(i*4);if(!found&&acc>low){nlo=lo+i*(hi-lo)/1023;found=true;}if(acc>=high){nhi=lo+(i+1)*(hi-lo)/1023;break;}}}
float oldlo=asfloat(O.Load(0)),oldhi=asfloat(O.Load(4));bool have=O.Load(12)!=0;
bool cut=!have||abs(nlo-oldlo)+abs(nhi-oldhi)>.6*(oldhi-oldlo)||flags==0||hi-lo<1e-6;
float alpha=saturate(1-exp(-p0/max(p1,1e-5)));if(!cut){nlo=lerp(oldlo,nlo,alpha);nhi=lerp(oldhi,nhi,alpha);}
O.Store(0,asuint(nlo));O.Store(4,asuint(nhi));O.Store(8,B.Load(8));O.Store(12,flags&&hi-lo>=1e-6?1:0);}
[numthreads(256,1,1)]void Normalize(uint3 t:SV_DispatchThreadID){if(t.x>=w*h)return;float lo=loadB(0),hi=loadB(1);O.Store(t.x*4,asuint(hi-lo>1e-6?saturate((loadA(t.x)-lo)/(hi-lo)):0));}
[numthreads(16,16,1)]void Dilate(uint3 t:SV_DispatchThreadID){if(t.x>=w||t.y>=h)return;float v=0;for(int k=-(int)radius;k<=(int)radius;k++){int2 p=clamp(int2(t.xy)+(axis==0?int2(k,0):int2(0,k)),0,int2(w-1,h-1));v=max(v,loadA(p.y*w+p.x));}O.Store((t.y*w+t.x)*4,asuint(v));}
[numthreads(256,1,1)]void Luma(uint3 t:SV_DispatchThreadID){if(t.x>=w*h)return;uint rgb=A.Load(t.x*4);float v=round(.299*(rgb&255)+.587*((rgb>>8)&255)+.114*((rgb>>16)&255));O.Store(t.x*4,asuint(v));}
[numthreads(256,1,1)]void PackLuma(uint3 t:SV_DispatchThreadID){uint row=t.x/(pitch/4),col=(t.x%(pitch/4))*4;if(row>=h)return;uint val=0;for(uint k=0;k<4;k++){uint x=col+k;uint l=x<w?(uint)clamp(loadA(row*w+x)+.5,0,255):0;val|=l<<(8*k);}O.Store(t.x*4,val);}
[numthreads(256,1,1)]void Input(uint3 t:SV_DispatchThreadID){if(t.x>=w*h)return;uint rgb=A.Load(t.x*4);float3 c=float3(rgb&255,(rgb>>8)&255,(rgb>>16)&255)/255;
if(flags)c=(c-float3(.485,.456,.406))/float3(.229,.224,.225);O.Store(t.x*4,asuint(c.x));O.Store((w*h+t.x)*4,asuint(c.y));O.Store((2*w*h+t.x)*4,asuint(c.z));}
float2 vectorAt(uint2 c){uint v=D.Load(c.y*pitch+c.x*4);return float2((int)(v<<16)>>16,(int)v>>16)*.25;}
float2 motion(uint2 xy){float2 p=clamp((float2(xy)+.5)/8-.5,0,float2(bw-1,bh-1));uint2 a=(uint2)p,b=min(a+1,uint2(bw-1,bh-1));float2 f=p-a;
return float2(xy)+lerp(lerp(vectorAt(a),vectorAt(uint2(b.x,a.y)),f.x),lerp(vectorAt(uint2(a.x,b.y)),vectorAt(b),f.x),f.y);}
[numthreads(16,16,1)]void Move(uint3 t:SV_DispatchThreadID){if(t.x>=w||t.y>=h)return;uint i=t.y*w+t.x;float2 p=motion(t.xy);float moved=sampleA(p,uint2(w,h));O.Store(i*4,asuint(moved));
if(flags){float other=sampleB(p,uint2(w,h));P.Store(i*4,asuint(other));}}
[numthreads(256,1,1)]void Difference(uint3 t:SV_DispatchThreadID){if(t.x>=w*h)return;O.Store(t.x*4,asuint(abs(loadA(t.x)-round(loadB(t.x)))/255));}
[numthreads(16,16,1)]void Blur(uint3 t:SV_DispatchThreadID){if(t.x>=w||t.y>=h)return;float sum=0,weight=0;for(int k=-(int)radius;k<=(int)radius;k++){
int2 q=int2(t.xy)+(axis==0?int2(k,0):int2(0,k));q=int2(reflect(q.x,w),reflect(q.y,h));float a=exp(-float(k*k)/(2*p0*p0));sum+=a*loadA(q.y*w+q.x);weight+=a;}O.Store((t.y*w+t.x)*4,asuint(sum/weight));}
[numthreads(256,1,1)]void Trust(uint3 t:SV_DispatchThreadID){if(t.x>=w*h)return;O.Store(t.x*4,asuint(saturate((.12-loadA(t.x))/.08)));}
groupshared float4 sums[256];
[numthreads(256,1,1)]void Sum(uint3 t:SV_DispatchThreadID,uint gi:SV_GroupIndex,uint3 g:SV_GroupID){uint i=t.x;float a=i<w*h?loadA(i):0,b=i<w*h?loadB(i):0;
sums[gi]=float4(a,b,a*a,a*b);GroupMemoryBarrierWithGroupSync();for(uint s=128;s>0;s>>=1){if(gi<s)sums[gi]+=sums[gi+s];GroupMemoryBarrierWithGroupSync();}if(gi==0)O.Store4(g.x*16,asuint(sums[0]));}
[numthreads(1,1,1)]void Fit(uint3 t:SV_DispatchThreadID){float4 sum=0;for(uint i=0;i<count;i++)sum+=asfloat(A.Load4(i*16));sum/=w*h;float a=(sum.w-sum.x*sum.y)/max(sum.z-sum.x*sum.x,1e-6);O.Store(0,asuint(a));O.Store(4,asuint(sum.y-a*sum.x));}
[numthreads(1,1,1)]void TrustMean(uint3 t:SV_DispatchThreadID){float sum=0;for(uint i=0;i<count;i++)sum+=asfloat(A.Load(i*16));O.Store(0,asuint(sum/(w*h)));}
[numthreads(256,1,1)]void Blend(uint3 t:SV_DispatchThreadID){if(t.x>=w*h)return;uint i=t.x;float trust=asfloat(D.Load(0))>=.5?asfloat(E.Load(i*4)):0;
float outv=mode==0?lerp(loadA(i),loadB(i),.5*trust):lerp(saturate(asfloat(P.Load(0))*loadA(i)+asfloat(P.Load(4))),loadB(i),trust);O.Store(i*4,asuint(outv));}
[numthreads(256,1,1)]void Affine(uint3 t:SV_DispatchThreadID){if(t.x<w*h)O.Store(t.x*4,asuint(saturate(loadB(0)*loadA(t.x)+loadB(1))));}
[numthreads(16,16,1)]void Area(uint3 t:SV_DispatchThreadID){if(t.x>=w||t.y>=h)return;float2 lo=float2(t.xy)*float2(sw,sh)/float2(w,h),hi=float2(t.xy+1)*float2(sw,sh)/float2(w,h);float z=0,d=0;
for(int y=(int)floor(lo.y);y<(int)ceil(hi.y);y++)for(int x=(int)floor(lo.x);x<(int)ceil(hi.x);x++){float a=(min(hi.x,x+1.)-max(lo.x,float(x)))*(min(hi.y,y+1.)-max(lo.y,float(y)))/((hi.x-lo.x)*(hi.y-lo.y));z+=a*loadA(y*sw+x);d+=a*loadB(y*sw+x);}
uint i=t.y*w+t.x;O.Store(i*4,asuint(z));P.Store(i*4,asuint(d));Q.Store(i*4,asuint(z*z));R.Store(i*4,asuint(z*d));}
[numthreads(256,1,1)]void Coeff(uint3 t:SV_DispatchThreadID){if(t.x>=w*h)return;uint i=t.x;float z=loadA(i),d=loadB(i),zz=asfloat(D.Load(i*4)),zd=asfloat(E.Load(i*4));float a=(zd-z*d+.002*asfloat(Q.Load(0)))/(zz-z*z+.002);O.Store(i*4,asuint(a));P.Store(i*4,asuint(d-a*z));}
[numthreads(16,16,1)]void ApplyFit(uint3 t:SV_DispatchThreadID){if(t.x>=w||t.y>=h)return;float2 p=(t.xy+.5)*float2(sw,sh)/float2(w,h)-.5;O.Store((t.y*w+t.x)*4,asuint(saturate(sampleA(p,uint2(sw,sh))*asfloat(D.Load((t.y*w+t.x)*4))+sampleB(p,uint2(sw,sh)))));}
[numthreads(16,16,1)]void Summary(uint3 t:SV_DispatchThreadID){if(t.x>=49||t.y>=28)return;O.Store((8+t.y*49+t.x)*4,asuint(sampleA((t.xy+.5)*float2(w,h)/float2(49,28)-.5,uint2(w,h))));if(t.x==0&&t.y==0){O.Store4(0,B.Load4(0));O.Store(28,flags?D.Load(8):0);}}
[numthreads(1,1,1)]void Quantiles(uint3 t:SV_DispatchThreadID){uint total=w*h,acc=0;bool a=false,b=false,c=false;for(uint i=0;i<1024;i++){acc+=A.Load(i*4);float v=i/1023.;if(!a&&acc>=max(1,(uint)(total*.1))){O.Store(16,asuint(v));a=true;}if(!b&&acc>=max(1,(uint)(total*.5))){O.Store(20,asuint(v));b=true;}if(!c&&acc>=max(1,(uint)(total*.9))){O.Store(24,asuint(v));c=true;}}}
// Crop fit is only 1232 pairs; a single GPU thread reproduces the robust fit.
[numthreads(1,1,1)]void CropFit(uint3 t:SV_DispatchThreadID){float scale=0,shift=0;bool good=D.Load(8)==0;for(uint fitIteration=0;fitIteration<2;fitIteration++){float sx=0,sy=0,sxx=0,syy=0,sxy=0,n=0;
for(uint y=2;y<30;y++)for(uint x=2;x<46;x++){float2 uv=(float2(x,y)+.5)/float2(48,32);float a=sampleA(uv*float2(w,h)-.5,uint2(w,h));float b=sampleB((float2(p0,p1)+uv*p2)*float2(w,h)-.5,uint2(w,h));if(fitIteration&&abs(scale*a+shift-b)>.15)continue;sx+=a;sy+=b;sxx+=a*a;syy+=b*b;sxy+=a*b;n++;}
float vx=sxx-sx*sx/max(n,1),vy=syy-sy*sy/max(n,1),cov=sxy-sx*sy/max(n,1);if(n<1232*.6||vx<1e-8||vy/max(n,1)<.002||cov<=0||cov*cov<.45*vx*vy){good=false;break;}scale=cov/vx;shift=(sy-scale*sx)/n;}
float error=0;for(uint yy=2;yy<30;yy++)for(uint xx=2;xx<46;xx++){float2 uv=(float2(xx,yy)+.5)/float2(48,32);float a=sampleA(uv*float2(w,h)-.5,uint2(w,h));float b=sampleB((float2(p0,p1)+uv*p2)*float2(w,h)-.5,uint2(w,h));float e=scale*a+shift-b;error+=min(.04,e*e);}if(sqrt(error/1232)>.12)good=false;
O.Store(0,asuint(scale));O.Store(4,asuint(shift));O.Store(8,good&&isfinite(scale)&&isfinite(shift)?1:0);}
[numthreads(16,16,1)]void CropApply(uint3 t:SV_DispatchThreadID){if(t.x>=w||t.y>=h)return;uint i=t.y*w+t.x;float original=loadB(i);float2 uv=((t.xy+.5)/float2(w,h)-float2(p0,p1))/p2;
if(D.Load(8)&&all(uv>0)&&all(uv<1)){float refined=saturate(asfloat(D.Load(0))*sampleA(uv*float2(w,h)-.5,uint2(w,h))+asfloat(D.Load(4)));float fg=saturate((max(original,refined)-.45)/.20),edge=saturate(min(min(uv.x,uv.y),min(1-uv.x,1-uv.y))/.12);original+=.65*fg*edge*clamp(refined-original,-.15,.15);}O.Store(i*4,asuint(original));}
)HLSL";

struct Constants {
  uint32_t w = 0, h = 0, sw = 0, sh = 0, mode = 0, radius = 0, axis = 0,
           flags = 0;
  float p0 = 0, p1 = 0, p2 = 0, p3 = 0, p4 = 0, p5 = 0, p6 = 0, p7 = 0;
  uint32_t pitch = 0, bw = 0, bh = 0, count = 0;
};

inline std::vector<std::pair<std::string, std::string>> ShaderJobs() {
  std::vector<std::pair<std::string, std::string>> jobs;
  for (const char *entry :
       {"Resample",   "RangeReduce",    "RangeFinish", "Clear",
        "Histogram",  "NormalizeRange", "Normalize",   "Dilate",
        "Luma",       "PackLuma",       "Input",       "Move",
        "Difference", "Blur",           "Trust",       "Sum",
        "Fit",        "TrustMean",      "Blend",       "Affine",
        "Area",       "Coeff",          "ApplyFit",    "Summary",
        "Quantiles",  "CropFit",        "CropApply"}) {
    jobs.push_back({std::string("gpu-depth-") + entry + ".hlsl",
                    std::string("#define ") + entry + " main\n" + Shader});
  }
  return jobs;
}

class Processor {
  ComPtr<ID3D12RootSignature> root_;
  std::map<std::string, ComPtr<ID3D12PipelineState>> shaders_;
  ComPtr<ID3D12Resource> dummy_, readback_;
  ComPtr<ID3D12QueryHeap> timestamps_;
  ComPtr<ID3D12Resource> timestampReadback_;
  UINT64 timestampFrequency_ = 0;
  using Compiler = bool (*)(const char *, const char *, ComPtr<ID3DBlob> &);
  Compiler compiler_ = nullptr;

public:
  ~Processor() {
    // work is declared before the image members and would otherwise be
    // destroyed after them. Retire submitted work before their ComPtrs are
    // released.
    try {
      if (work.fence)
        work.Wait();
    } catch (...) {
    }
  }
  void SetCompiler(Compiler compiler) { compiler_ = compiler; }
  Queue work;
  int width = 0, height = 0;
  ComPtr<ID3D12Resource> raw, nearMap, finalMap, range, reduction, histogram,
      summary;
  ComPtr<ID3D12Resource> luma, previous, previousLuma, anchor, anchorLuma,
      anchorFit;
  std::array<ComPtr<ID3D12Resource>, 16> scratch;
  void Init(ID3D12Device *device, int w, int h) {
    width = w;
    height = h;
    work.Init(device);
    D3D12_ROOT_PARAMETER rp[9]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants = {0, 0, sizeof(Constants) / 4};
    for (int i = 0; i < 8; i++) {
      rp[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
      rp[i + 1].Descriptor.ShaderRegister = i;
    }
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 9;
    rd.pParameters = rp;
    ComPtr<ID3DBlob> blob, err;
    Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob,
                                      &err),
          "serialize root");
    Check(device->CreateRootSignature(0, blob->GetBufferPointer(),
                                      blob->GetBufferSize(),
                                      IID_PPV_ARGS(&root_)),
          "root signature");
    const UINT64 bytes = UINT64(w) * h * 4;
    for (auto *b : {std::addressof(raw), std::addressof(nearMap),
                    std::addressof(finalMap), std::addressof(luma),
                    std::addressof(previous), std::addressof(previousLuma),
                    std::addressof(anchor), std::addressof(anchorLuma)})
      *b = Buffer(device, bytes);
    for (auto &b : scratch)
      b = Buffer(device, bytes);
    range = Buffer(device, 16);
    anchorFit = Buffer(device, 16);
    reduction = Buffer(device, ((UINT64(w) * h + 255) / 256) * 16);
    histogram = Buffer(device, 4096);
    summary = Buffer(device, (8 + 49 * 28) * 4);
    readback_ = Buffer(device, (8 + 49 * 28) * 4, D3D12_HEAP_TYPE_READBACK);
    dummy_ = Buffer(device, 16);
    D3D12_QUERY_HEAP_DESC query{};
    query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query.Count = 4;
    Check(device->CreateQueryHeap(&query, IID_PPV_ARGS(&timestamps_)),
          "depth timestamps");
    timestampReadback_ =
        Buffer(device, 4 * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK);
    Check(work.queue->GetTimestampFrequency(&timestampFrequency_),
          "depth timestamp frequency");
    work.Begin();
    Constants c{};
    c.count = 4;
    Dispatch("Clear", c, {}, {range.Get()});
    Dispatch("Clear", c, {}, {anchorFit.Get()});
    work.Submit();
    work.Wait();
  }
  ComPtr<ID3D12Resource> Image() {
    return Buffer(work.device.Get(), UINT64(width) * height * 4);
  }
  Constants Size(int w = 0, int h = 0) const {
    Constants c;
    c.w = w ? w : width;
    c.h = h ? h : height;
    return c;
  }
  void BeginStage(unsigned stage) {
    work.list->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                        stage * 2);
  }
  void EndStage(unsigned stage) {
    work.list->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                        stage * 2 + 1);
    work.list->ResolveQueryData(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                stage * 2, 2, timestampReadback_.Get(),
                                stage * 2 * sizeof(UINT64));
  }
  std::array<double, 2> StageMilliseconds() {
    work.Wait();
    void *mapped = nullptr;
    Check(timestampReadback_->Map(0, nullptr, &mapped), "depth timestamps map");
    const auto *t = static_cast<const UINT64 *>(mapped);
    std::array<double, 2> result{};
    for (unsigned i = 0; i < 2; i++)
      if (t[i * 2 + 1] >= t[i * 2])
        result[i] =
            1000.0 * double(t[i * 2 + 1] - t[i * 2]) / timestampFrequency_;
    timestampReadback_->Unmap(0, nullptr);
    return result;
  }
  void Dispatch(const char *entry, const Constants &c,
                std::initializer_list<ID3D12Resource *> inputs,
                std::initializer_list<ID3D12Resource *> outputs) {
    auto &pso = shaders_[entry];
    if (!pso) {
      ComPtr<ID3DBlob> code, error;
      HRESULT hr = S_OK;
      if (compiler_) {
        const std::string name = std::string("gpu-depth-") + entry + ".hlsl";
        const std::string source =
            std::string("#define ") + entry + " main\n" + Shader;
        if (!compiler_(name.c_str(), source.c_str(), code))
          hr = E_FAIL;
      } else {
        hr = D3DCompile(Shader, strlen(Shader), "gpu-depth", nullptr, nullptr,
                        entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                        &code, &error);
      }
      if (FAILED(hr))
        throw std::runtime_error(
            std::string("GPU depth shader ") + entry + ": " +
            (error ? (const char *)error->GetBufferPointer()
                   : "compile failed"));
      D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
      pd.pRootSignature = root_.Get();
      pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
      Check(work.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)),
            "compute pipeline");
    }
    auto cl = work.list.Get();
    cl->SetComputeRootSignature(root_.Get());
    cl->SetPipelineState(pso.Get());
    cl->SetComputeRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
    for (int i = 0; i < 8; i++)
      cl->SetComputeRootUnorderedAccessView(i + 1,
                                            dummy_->GetGPUVirtualAddress());
    int i = 0;
    for (auto *r : inputs)
      cl->SetComputeRootUnorderedAccessView(
          ++i, (r ? r : dummy_.Get())->GetGPUVirtualAddress());
    i = 4;
    for (auto *r : outputs)
      cl->SetComputeRootUnorderedAccessView(
          ++i, (r ? r : dummy_.Get())->GetGPUVirtualAddress());
    std::string name = entry;
    bool one = name == "RangeFinish" || name == "NormalizeRange" ||
               name == "Fit" || name == "TrustMean" || name == "CropFit" ||
               name == "Quantiles";
    bool two = name == "Resample" || name == "Dilate" || name == "Move" ||
               name == "Blur" || name == "Area" || name == "ApplyFit" ||
               name == "CropApply" || name == "Summary";
    if (one)
      cl->Dispatch(1, 1, 1);
    else if (two)
      cl->Dispatch(((name == "Summary" ? 49 : c.w) + 15) / 16,
                   ((name == "Summary" ? 28 : c.h) + 15) / 16, 1);
    else
      cl->Dispatch(((name == "Clear"      ? c.count
                     : name == "PackLuma" ? c.pitch / 4 * c.h
                                          : c.w * c.h) +
                    255) /
                       256,
                   1, 1);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cl->ResourceBarrier(1, &b);
  }
  void Normalize(ID3D12Resource *input, int sw, int sh, float elapsed,
                 float tau, bool smooth) {
    Constants c = Size();
    c.sw = sw;
    c.sh = sh;
    Dispatch("Resample", c, {input}, {raw.Get()});
    Dispatch("RangeReduce", c, {raw.Get()}, {reduction.Get()});
    c.count = (width * height + 255) / 256;
    Dispatch("RangeFinish", c, {reduction.Get()}, {scratch[0].Get()});
    c.count = 1024;
    Dispatch("Clear", c, {}, {histogram.Get()});
    Dispatch("Histogram", c, {raw.Get(), scratch[0].Get()}, {histogram.Get()});
    c.flags = smooth ? 1 : 0;
    c.p0 = elapsed;
    c.p1 = tau;
    Dispatch("NormalizeRange", c, {histogram.Get(), scratch[0].Get()},
             {range.Get()});
    Dispatch("Normalize", c, {raw.Get(), range.Get()}, {nearMap.Get()});
  }
  void Blur(ID3D12Resource *input, ID3D12Resource *output, ID3D12Resource *temp,
            int w, int h, float sigma) {
    Constants c = Size(w, h);
    c.radius = (uint32_t)std::lround(sigma * 4);
    c.p0 = sigma;
    Dispatch("Blur", c, {input}, {temp});
    c.axis = 1;
    Dispatch("Blur", c, {temp}, {output});
  }
  void Fit(ID3D12Resource *z, ID3D12Resource *d, ID3D12Resource *output) {
    Constants c = Size();
    Dispatch("Sum", c, {z, d}, {reduction.Get()});
    c.count = (width * height + 255) / 256;
    Dispatch("Fit", c, {reduction.Get()}, {output});
  }
  // slots 0..5 are remap/trust temporaries; slots 6..15 are guided-fit work.
  void MoveTrust(ID3D12Resource *reference, ID3D12Resource *refLuma,
                 ID3D12Resource *vectors, int pitch, int bw, int bh) {
    Constants c = Size();
    c.pitch = pitch;
    c.bw = bw;
    c.bh = bh;
    c.flags = 1;
    Dispatch("Move", c, {reference, refLuma, vectors},
             {scratch[0].Get(), scratch[1].Get()});
    Dispatch("Difference", c, {luma.Get(), scratch[1].Get()},
             {scratch[2].Get()});
    Blur(scratch[2].Get(), scratch[3].Get(), scratch[4].Get(), width, height,
         4);
    Dispatch("Trust", c, {scratch[3].Get()}, {scratch[2].Get()});
    Dispatch("Sum", c, {scratch[2].Get(), scratch[2].Get()}, {reduction.Get()});
    c.count = (width * height + 255) / 256;
    Dispatch("TrustMean", c, {reduction.Get()}, {scratch[5].Get()});
  }
  void Steady(ID3D12Resource *vectors, int pitch, int bw, int bh) {
    MoveTrust(previous.Get(), previousLuma.Get(), vectors, pitch, bw, bh);
    Constants c = Size();
    Dispatch(
        "Blend", c,
        {nearMap.Get(), scratch[0].Get(), scratch[5].Get(), scratch[2].Get()},
        {finalMap.Get()});
    work.Copy(nearMap.Get(), finalMap.Get(), UINT64(width) * height * 4);
  }
  void Fuse(ID3D12Resource *vectors, int pitch, int bw, int bh, bool sameFrame,
            bool usable) {
    Constants c = Size();
    if (!usable) {
      Dispatch("Affine", c, {nearMap.Get(), anchorFit.Get()}, {finalMap.Get()});
      work.Copy(nearMap.Get(), finalMap.Get(), UINT64(width) * height * 4);
      return;
    }
    if (!sameFrame)
      MoveTrust(anchor.Get(), anchorLuma.Get(), vectors, pitch, bw, bh);
    if (sameFrame)
      work.Copy(scratch[6].Get(), anchor.Get(), UINT64(width) * height * 4);
    else {
      c.mode = 1;
      Dispatch(
          "Blend", c,
          {nearMap.Get(), scratch[0].Get(), scratch[5].Get(), scratch[2].Get()},
          {scratch[6].Get(), anchorFit.Get()});
    }
    Fit(nearMap.Get(), scratch[6].Get(), scratch[15].Get());
    c.w = width / 4;
    c.h = height / 4;
    c.sw = width;
    c.sh = height;
    Dispatch("Area", c, {nearMap.Get(), scratch[6].Get()},
             {scratch[7].Get(), scratch[8].Get(), scratch[9].Get(),
              scratch[10].Get()});
    for (int k = 7; k <= 10; k++) {
      Blur(scratch[k].Get(), scratch[k].Get(), scratch[11].Get(), c.w, c.h, 4);
    }
    Dispatch("Coeff", c,
             {scratch[7].Get(), scratch[8].Get(), scratch[9].Get(),
              scratch[10].Get()},
             {scratch[12].Get(), scratch[13].Get(), scratch[15].Get()});
    Blur(scratch[12].Get(), scratch[12].Get(), scratch[11].Get(), c.w, c.h, 4);
    Blur(scratch[13].Get(), scratch[13].Get(), scratch[11].Get(), c.w, c.h, 4);
    c.sw = c.w;
    c.sh = c.h;
    c.w = width;
    c.h = height;
    Dispatch("ApplyFit", c,
             {scratch[12].Get(), scratch[13].Get(), nearMap.Get()},
             {finalMap.Get()});
    work.Copy(nearMap.Get(), finalMap.Get(), UINT64(width) * height * 4);
  }
  void Finish(int rx, int ry, bool crop = false) {
    Constants c = Size();
    c.flags = crop ? 1 : 0;
    work.Copy(previous.Get(), nearMap.Get(), UINT64(width) * height * 4);
    work.Copy(previousLuma.Get(), luma.Get(), UINT64(width) * height * 4);
    c.radius = std::max(0, rx);
    Dispatch("Dilate", c, {nearMap.Get()}, {scratch[14].Get()});
    c.radius = std::max(0, ry);
    c.axis = 1;
    Dispatch("Dilate", c, {scratch[14].Get()}, {finalMap.Get()});
    c.count = 1024;
    Dispatch("Clear", c, {}, {histogram.Get()});
    c.mode = 1;
    Dispatch("Histogram", c, {nearMap.Get(), range.Get()}, {histogram.Get()});
    Dispatch("Quantiles", c, {histogram.Get()}, {summary.Get()});
    Dispatch("Summary", c, {nearMap.Get(), range.Get(), scratch[15].Get()},
             {summary.Get()});
    work.Copy(readback_.Get(), summary.Get(), (8 + 49 * 28) * 4,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_COPY_DEST);
  }
  struct Summary {
    float lo = 0, hi = 1, p10 = 0, p50 = 0, p90 = 0;
    uint32_t invalid = 0, cropAccepted = 0;
    std::vector<float> tracker;
  };
  Summary ReadSummary() {
    work.Wait();
    Summary s;
    void *p = nullptr;
    Check(readback_->Map(0, nullptr, &p), "summary map");
    auto *f = (float *)p;
    s.lo = f[0];
    s.hi = f[1];
    memcpy(&s.invalid, f + 2, 4);
    s.p10 = f[4];
    s.p50 = f[5];
    s.p90 = f[6];
    memcpy(&s.cropAccepted, f + 7, 4);
    s.tracker.assign(f + 8, f + 8 + 49 * 28);
    readback_->Unmap(0, nullptr);
    return s;
  }
  std::vector<float> Download(ID3D12Resource *source) {
    auto rb = Buffer(work.device.Get(), UINT64(width) * height * 4,
                     D3D12_HEAP_TYPE_READBACK);
    work.Begin();
    work.Copy(rb.Get(), source, UINT64(width) * height * 4,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_COPY_DEST);
    work.Submit();
    work.Wait();
    void *p = nullptr;
    Check(rb->Map(0, nullptr, &p), "diagnostic map");
    std::vector<float> v((float *)p, (float *)p + width * height);
    rb->Unmap(0, nullptr);
    return v;
  }
};
} // namespace gpudepth
