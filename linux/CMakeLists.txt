cmake_minimum_required(VERSION 3.16)
project(vrx_linux LANGUAGES CXX)

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

add_executable(vrx-probe probe.cpp)
target_compile_features(vrx-probe PRIVATE cxx_std_17)
target_include_directories(vrx-probe PRIVATE ../bench/native/openxr/include)
target_link_libraries(vrx-probe PRIVATE Vulkan::Vulkan ${CMAKE_DL_LIBS})

find_program(GLSLANG_VALIDATOR glslangValidator)
if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "glslangValidator is required to build the Linux stereo shader")
endif()
set(STEREO_SPV "${CMAKE_CURRENT_BINARY_DIR}/stereo_warp.comp.spv")
add_custom_command(
    OUTPUT "${STEREO_SPV}"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 -S comp
            "${CMAKE_CURRENT_SOURCE_DIR}/stereo_warp.comp" -o "${STEREO_SPV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/stereo_warp.comp"
    VERBATIM
)
add_custom_target(vrx-stereo-shader DEPENDS "${STEREO_SPV}")
set(PREP_SPV "${CMAKE_CURRENT_BINARY_DIR}/model_prep.comp.spv")
add_custom_command(
    OUTPUT "${PREP_SPV}"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 -S comp
            "${CMAKE_CURRENT_SOURCE_DIR}/model_prep.comp" -o "${PREP_SPV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/model_prep.comp"
    VERBATIM
)
add_custom_target(vrx-model-prep-shader DEPENDS "${PREP_SPV}")

set(CAPTURE_SCALE_SPV "${CMAKE_CURRENT_BINARY_DIR}/capture_scale.comp.spv")
add_custom_command(
    OUTPUT "${CAPTURE_SCALE_SPV}"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 -S comp
            "${CMAKE_CURRENT_SOURCE_DIR}/capture_scale.comp" -o "${CAPTURE_SCALE_SPV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/capture_scale.comp"
    VERBATIM
)
add_custom_target(vrx-capture-scale-shader DEPENDS "${CAPTURE_SCALE_SPV}")

set(ROOM_MIRROR_SPV "${CMAKE_CURRENT_BINARY_DIR}/room_mirror.comp.spv")
add_custom_command(
    OUTPUT "${ROOM_MIRROR_SPV}"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 -S comp
            "${CMAKE_CURRENT_SOURCE_DIR}/room_mirror.comp" -o "${ROOM_MIRROR_SPV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/room_mirror.comp"
    VERBATIM
)
add_custom_target(vrx-room-mirror-shader ALL DEPENDS "${ROOM_MIRROR_SPV}")

set(ROOM_EMIT_SPV "${CMAKE_CURRENT_BINARY_DIR}/room_emit.comp.spv")
add_custom_command(
    OUTPUT "${ROOM_EMIT_SPV}"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 -S comp
            "${CMAKE_CURRENT_SOURCE_DIR}/room_emit.comp" -o "${ROOM_EMIT_SPV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/room_emit.comp"
    VERBATIM
)
add_custom_target(vrx-room-emit-shader ALL DEPENDS "${ROOM_EMIT_SPV}")

set(ROOM_LIGHT_SPV "${CMAKE_CURRENT_BINARY_DIR}/room_light.comp.spv")
add_custom_command(
    OUTPUT "${ROOM_LIGHT_SPV}"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 -S comp
            "${CMAKE_CURRENT_SOURCE_DIR}/room_light.comp" -o "${ROOM_LIGHT_SPV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/room_light.comp"
    VERBATIM
)
add_custom_target(vrx-room-light-shader ALL DEPENDS "${ROOM_LIGHT_SPV}")

add_executable(vrx-room-eye-source room_eye_source.cpp)
target_compile_features(vrx-room-eye-source PRIVATE cxx_std_17)
target_include_directories(vrx-room-eye-source PRIVATE ../bench/native/openxr/include ../bench/native/openxr)
set(ROOM_EYE_HLSL "${CMAKE_CURRENT_BINARY_DIR}/room_eye.comp.hlsl")
set(ROOM_EYE_SPV "${CMAKE_CURRENT_BINARY_DIR}/room_eye.comp.spv")
add_custom_command(
    OUTPUT "${ROOM_EYE_HLSL}"
    COMMAND vrx-room-eye-source "${CMAKE_CURRENT_SOURCE_DIR}/../bench/native/openxr/xrapp5.cpp" "${ROOM_EYE_HLSL}"
    DEPENDS vrx-room-eye-source "${CMAKE_CURRENT_SOURCE_DIR}/../bench/native/openxr/xrapp5.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../bench/native/openxr/room.h"
    VERBATIM
)
add_custom_command(
    OUTPUT "${ROOM_EYE_SPV}"
    COMMAND "${GLSLANG_VALIDATOR}" -D -S comp -V --target-env vulkan1.1
            --shift-sampler-binding comp 4 --shift-image-binding comp 5
            --shift-ubo-binding comp 6 -e main -o "${ROOM_EYE_SPV}" "${ROOM_EYE_HLSL}"
    DEPENDS "${ROOM_EYE_HLSL}"
    VERBATIM
)
add_custom_target(vrx-room-eye-shader ALL DEPENDS "${ROOM_EYE_SPV}")

add_executable(vrx-xr-synthetic xr_synthetic.cpp vulkan_warp.cpp vulkan_prep.cpp vulkan_room.cpp model_prep_cpu.cpp
    vulkan_capture_scale.cpp vulkan_dmabuf.cpp)
add_dependencies(vrx-xr-synthetic vrx-stereo-shader vrx-model-prep-shader vrx-capture-scale-shader
    vrx-room-mirror-shader vrx-room-emit-shader vrx-room-light-shader vrx-room-eye-shader)
target_compile_definitions(vrx-xr-synthetic PRIVATE VRX_STEREO_SPV_PATH="${STEREO_SPV}" VRX_PREP_SPV_PATH="${PREP_SPV}"
    VRX_CAPTURE_SCALE_SPV_PATH="${CAPTURE_SCALE_SPV}"
    VRX_ROOM_MIRROR_SPV_PATH="${ROOM_MIRROR_SPV}" VRX_ROOM_EMIT_SPV_PATH="${ROOM_EMIT_SPV}"
    VRX_ROOM_LIGHT_SPV_PATH="${ROOM_LIGHT_SPV}" VRX_ROOM_EYE_SPV_PATH="${ROOM_EYE_SPV}")
target_compile_features(vrx-xr-synthetic PRIVATE cxx_std_17)
target_include_directories(vrx-xr-synthetic PRIVATE ../bench/native/openxr/include ../bench/native/openxr)
target_link_libraries(vrx-xr-synthetic PRIVATE Vulkan::Vulkan Threads::Threads ${CMAKE_DL_LIBS})

add_executable(vrx-capture-gpu-test capture_gpu_test.cpp vulkan_capture_scale.cpp vulkan_prep.cpp model_prep_cpu.cpp)
add_dependencies(vrx-capture-gpu-test vrx-capture-scale-shader vrx-model-prep-shader)
target_compile_definitions(vrx-capture-gpu-test PRIVATE VRX_CAPTURE_SCALE_SPV_PATH="${CAPTURE_SCALE_SPV}"
    VRX_PREP_SPV_PATH="${PREP_SPV}")
target_compile_features(vrx-capture-gpu-test PRIVATE cxx_std_17)
target_include_directories(vrx-capture-gpu-test PRIVATE ../bench/native/openxr)
target_link_libraries(vrx-capture-gpu-test PRIVATE Vulkan::Vulkan Threads::Threads)

add_executable(vrx-room-gpu-probe room_gpu_probe.cpp)
add_dependencies(vrx-room-gpu-probe vrx-room-mirror-shader vrx-room-emit-shader vrx-room-light-shader)
target_compile_features(vrx-room-gpu-probe PRIVATE cxx_std_17)
target_compile_definitions(vrx-room-gpu-probe PRIVATE VRX_ROOM_MIRROR_SPV_PATH="${ROOM_MIRROR_SPV}" VRX_ROOM_EMIT_SPV_PATH="${ROOM_EMIT_SPV}" VRX_ROOM_LIGHT_SPV_PATH="${ROOM_LIGHT_SPV}")
target_include_directories(vrx-room-gpu-probe PRIVATE ../bench/native/openxr/include ../bench/native/openxr)
target_link_libraries(vrx-room-gpu-probe PRIVATE Vulkan::Vulkan)

add_executable(vrx-depth-range-test depth_range_test.cpp)
target_compile_features(vrx-depth-range-test PRIVATE cxx_std_17)

add_executable(vrx-capture-scale-test capture_scale_test.cpp)
target_compile_features(vrx-capture-scale-test PRIVATE cxx_std_17)
target_include_directories(vrx-capture-scale-test PRIVATE ../bench/native/openxr)

add_executable(vrx-room-reference room_reference.cpp)
target_compile_features(vrx-room-reference PRIVATE cxx_std_17)
target_include_directories(vrx-room-reference PRIVATE ../bench/native/openxr/include ../bench/native/openxr)

find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h PATH_SUFFIXES onnxruntime)
find_library(ORT_LIBRARY NAMES onnxruntime)
if(ORT_INCLUDE_DIR AND ORT_LIBRARY)
    add_executable(vrx-model-probe model_probe.cpp)
    target_compile_features(vrx-model-probe PRIVATE cxx_std_17)
    target_include_directories(vrx-model-probe PRIVATE "${ORT_INCLUDE_DIR}")
    target_link_libraries(vrx-model-probe PRIVATE "${ORT_LIBRARY}")
else()
    message(STATUS "ONNX Runtime development files unavailable; vrx-model-probe is disabled")
endif()

find_package(PNG QUIET)
if(TARGET vrx-model-probe AND PNG_FOUND)
    target_sources(vrx-xr-synthetic PRIVATE model_depth.cpp still_image.cpp)
    target_include_directories(vrx-xr-synthetic PRIVATE "${ORT_INCLUDE_DIR}")
    target_compile_definitions(vrx-xr-synthetic PRIVATE VRX_HAS_MODEL=1)
    target_link_libraries(vrx-xr-synthetic PRIVATE "${ORT_LIBRARY}" PNG::PNG)
else()
    message(STATUS "Still-image model mode requires ONNX Runtime development files and libpng")
endif()

if(PkgConfig_FOUND)
    pkg_check_modules(GIO QUIET IMPORTED_TARGET gio-2.0)
endif()
find_path(PORTAL_INCLUDE_DIR libportal/portal.h)
find_library(PORTAL_LIBRARY NAMES portal)
find_path(PIPEWIRE_INCLUDE_DIR pipewire/pipewire.h PATH_SUFFIXES pipewire-0.3)
find_path(SPA_INCLUDE_DIR spa/param/video/raw-utils.h PATH_SUFFIXES spa-0.2)
find_library(PIPEWIRE_LIBRARY NAMES pipewire-0.3)
if(TARGET PkgConfig::GIO AND PORTAL_INCLUDE_DIR AND PORTAL_LIBRARY AND
   PIPEWIRE_INCLUDE_DIR AND SPA_INCLUDE_DIR AND PIPEWIRE_LIBRARY)
    add_executable(vrx-capture-probe capture_probe.cpp vulkan_dmabuf.cpp)
    target_compile_features(vrx-capture-probe PRIVATE cxx_std_17)
    target_include_directories(vrx-capture-probe PRIVATE
        "${PORTAL_INCLUDE_DIR}" "${PIPEWIRE_INCLUDE_DIR}" "${SPA_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../bench/native/openxr")
    target_link_libraries(vrx-capture-probe PRIVATE
        PkgConfig::GIO "${PORTAL_LIBRARY}" "${PIPEWIRE_LIBRARY}" Vulkan::Vulkan)
    target_sources(vrx-xr-synthetic PRIVATE portal_capture.cpp drm_timeline.cpp)
    target_include_directories(vrx-xr-synthetic PRIVATE
        "${PORTAL_INCLUDE_DIR}" "${PIPEWIRE_INCLUDE_DIR}" "${SPA_INCLUDE_DIR}")
    target_link_libraries(vrx-xr-synthetic PRIVATE
        PkgConfig::GIO "${PORTAL_LIBRARY}" "${PIPEWIRE_LIBRARY}")
    target_compile_definitions(vrx-xr-synthetic PRIVATE VRX_HAS_CAPTURE=1)
    if(TARGET vrx-model-probe AND PNG_FOUND)
        target_sources(vrx-xr-synthetic PRIVATE live_depth.cpp)
        target_compile_definitions(vrx-xr-synthetic PRIVATE VRX_HAS_LIVE_DEPTH=1)
    endif()
else()
    message(STATUS "libportal/PipeWire development files unavailable; vrx-capture-probe is disabled")
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME vrx-probe-help COMMAND vrx-probe --help)
    add_test(NAME vrx-xr-synthetic-help COMMAND vrx-xr-synthetic --help)
    add_test(NAME vrx-room-reference COMMAND vrx-room-reference)
    add_test(NAME vrx-capture-scale COMMAND vrx-capture-scale-test)
    add_test(NAME vrx-depth-range COMMAND vrx-depth-range-test)
    option(VRX_TEST_VULKAN_GPU "Run hardware Vulkan probes in CTest" OFF)
    if(VRX_TEST_VULKAN_GPU)
        add_test(NAME vrx-room-gpu-probe COMMAND vrx-room-gpu-probe)
        add_test(NAME vrx-capture-gpu-test COMMAND vrx-capture-gpu-test)
    endif()
    if(TARGET vrx-capture-probe)
        add_test(NAME vrx-capture-probe-help COMMAND vrx-capture-probe --help)
    endif()
    if(TARGET vrx-model-probe)
        add_test(NAME vrx-model-probe-help COMMAND vrx-model-probe --help)
        set(ZIPDEPTH_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/../bench/models/zipdepth_faithful_fp16_672x384.onnx")
        if(EXISTS "${ZIPDEPTH_MODEL}")
            add_test(NAME vrx-model-probe-cpu COMMAND vrx-model-probe "${ZIPDEPTH_MODEL}")
        endif()
    endif()
endif()
