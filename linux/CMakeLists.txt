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

add_executable(vrx-xr-synthetic xr_synthetic.cpp vulkan_warp.cpp vulkan_prep.cpp)
add_dependencies(vrx-xr-synthetic vrx-stereo-shader vrx-model-prep-shader)
target_compile_definitions(vrx-xr-synthetic PRIVATE VRX_STEREO_SPV_PATH="${STEREO_SPV}" VRX_PREP_SPV_PATH="${PREP_SPV}")
target_compile_features(vrx-xr-synthetic PRIVATE cxx_std_17)
target_include_directories(vrx-xr-synthetic PRIVATE ../bench/native/openxr/include ../bench/native/openxr)
target_link_libraries(vrx-xr-synthetic PRIVATE Vulkan::Vulkan Threads::Threads ${CMAKE_DL_LIBS})

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
    add_executable(vrx-capture-probe capture_probe.cpp)
    target_compile_features(vrx-capture-probe PRIVATE cxx_std_17)
    target_include_directories(vrx-capture-probe PRIVATE
        "${PORTAL_INCLUDE_DIR}" "${PIPEWIRE_INCLUDE_DIR}" "${SPA_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../bench/native/openxr")
    target_link_libraries(vrx-capture-probe PRIVATE
        PkgConfig::GIO "${PORTAL_LIBRARY}" "${PIPEWIRE_LIBRARY}")
else()
    message(STATUS "libportal/PipeWire development files unavailable; vrx-capture-probe is disabled")
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME vrx-probe-help COMMAND vrx-probe --help)
    add_test(NAME vrx-xr-synthetic-help COMMAND vrx-xr-synthetic --help)
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
