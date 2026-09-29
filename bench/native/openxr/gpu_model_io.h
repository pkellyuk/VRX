#pragma once
#include "dml_provider_factory.h"
#include "gpu_depth.h"
#include <onnxruntime_c_api.h>

namespace gpudepth {
inline void OrtCheck(const OrtApi *api, OrtStatus *status,
                     const char *operation) {
  if (!status)
    return;
  const std::string message =
      std::string(operation) + ": " + api->GetErrorMessage(status);
  api->ReleaseStatus(status);
  throw std::runtime_error(message);
}

// Owns both the DML wrapper and tensor for an externally allocated GPU buffer.
// The session/device must outlive this object. Shape/type is taken from the
// model.
class ModelOutput {
  const OrtApi *api_ = nullptr;
  const OrtDmlApi *dml_ = nullptr;
  void *allocation_ = nullptr;
  OrtValue *value_ = nullptr;
  OrtIoBinding *binding_ = nullptr;

public:
  ComPtr<ID3D12Resource> resource;
  ~ModelOutput() { Release(); }
  void Release() {
    if (binding_) {
      // A failed run may still have submitted commands using this allocation.
      if (auto *status = api_->SynchronizeBoundOutputs(binding_))
        api_->ReleaseStatus(status);
    }
    if (binding_)
      api_->ReleaseIoBinding(binding_);
    if (value_)
      api_->ReleaseValue(value_);
    if (allocation_) {
      if (auto *s = dml_->FreeGPUAllocation(allocation_))
        api_->ReleaseStatus(s);
    }
    binding_ = nullptr;
    value_ = nullptr;
    allocation_ = nullptr;
    resource.Reset();
  }
  void Init(const OrtApi *api, const OrtDmlApi *dml, OrtSession *session,
            ID3D12Device *device, OrtValue *input, const char *inputName,
            const char *outputName, size_t elements) {
    Release();
    api_ = api;
    dml_ = dml;
    OrtTypeInfo *type = nullptr;
    OrtCheck(api, api->SessionGetOutputTypeInfo(session, 0, &type),
             "output type");
    const OrtTensorTypeAndShapeInfo *tensor = nullptr;
    OrtStatus *status = api->CastTypeInfoToTensorInfo(type, &tensor);
    size_t dimensions = 0, count = 0;
    ONNXTensorElementDataType elementType;
    if (!status)
      status = api->GetDimensionsCount(tensor, &dimensions);
    std::vector<int64_t> shape(dimensions);
    if (!status)
      status = api->GetDimensions(tensor, shape.data(), shape.size());
    if (!status)
      status = api->GetTensorShapeElementCount(tensor, &count);
    if (!status)
      status = api->GetTensorElementType(tensor, &elementType);
    api->ReleaseTypeInfo(type);
    OrtCheck(api, status, "GPU output shape");
    if (count != elements || elementType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
      throw std::runtime_error("GPU depth: unsupported output shape/type");
    resource = Buffer(device, elements * sizeof(float));
    OrtCheck(
        api,
        dml->CreateGPUAllocationFromD3DResource(resource.Get(), &allocation_),
        "GPU output allocation");
    OrtMemoryInfo *memory = nullptr;
    OrtCheck(api,
             api->CreateMemoryInfo("DML", OrtDeviceAllocator, 0,
                                   OrtMemTypeDefault, &memory),
             "DML output memory");
    status = api->CreateTensorWithDataAsOrtValue(
        memory, allocation_, elements * sizeof(float), shape.data(),
        shape.size(), elementType, &value_);
    api->ReleaseMemoryInfo(memory);
    OrtCheck(api, status, "GPU output tensor");
    OrtCheck(api, api->CreateIoBinding(session, &binding_), "GPU I/O binding");
    OrtCheck(api, api->BindInput(binding_, inputName, input), "bind GPU input");
    OrtCheck(api, api->BindOutput(binding_, outputName, value_),
             "bind GPU output");
  }
  void Run(OrtSession *session) {
    OrtCheck(api_, api_->RunWithBinding(session, nullptr, binding_),
             "GPU inference");
    Synchronize();
  }
  void Synchronize() {
    if (binding_)
      OrtCheck(api_, api_->SynchronizeBoundOutputs(binding_),
               "GPU inference completion");
  }
};
} // namespace gpudepth
