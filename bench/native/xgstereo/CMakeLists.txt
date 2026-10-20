cmake_minimum_required(VERSION 3.24)
project(vrx_xgstereo LANGUAGES CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This prototype requires 64-bit Windows.")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
# Headers only. No SDK, system layer registration, or changes to installed games.
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG 354fab82dbd91d2526a41ee4dbfff1012a623798)
FetchContent_Declare(spirv_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
    GIT_TAG 04f10f650d514df88b76d25e83db360142c7b174)
FetchContent_MakeAvailable(vulkan_headers spirv_headers)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen_chain_sizes.py"
    "${vulkan_headers_SOURCE_DIR}/registry/vk.xml" "${CMAKE_CURRENT_BINARY_DIR}/chain_sizes.inc"
    COMMAND_ERROR_IS_FATAL ANY)

execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen_mirror.py"
    "${vulkan_headers_SOURCE_DIR}/registry/vk.xml" "${CMAKE_CURRENT_BINARY_DIR}/mirror_dispatch.hpp"
    "${CMAKE_CURRENT_BINARY_DIR}/mirror_gen.hpp" COMMAND_ERROR_IS_FATAL ANY)

option(VRX_XG_VALIDATE_SPIRV "Validate test shaders against the Vulkan SPIR-V rules" ON)
if(VRX_XG_VALIDATE_SPIRV)
    set(SPIRV-Headers_SOURCE_DIR "${spirv_headers_SOURCE_DIR}")
    set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
    set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
    set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(spirv_tools
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
        GIT_TAG fbe4f3ad913c44fe8700545f8ffe35d1382b7093)
    FetchContent_MakeAvailable(spirv_tools)
endif()

add_library(vrx_xgstereo SHARED layer.cpp layer.def)
# Games can preload an old app-local MSVC runtime. In particular, their older
# mutex implementation cannot use the constexpr mutex layout in newer headers.
# Keep all C++ allocations/runtime state inside the layer; only Vulkan's C ABI
# crosses the boundary, so the layer can safely carry its own static runtime.
set_property(TARGET vrx_xgstereo PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(vrx_xgstereo PRIVATE /Zi)
    target_link_options(vrx_xgstereo PRIVATE /DEBUG:FULL)
endif()
target_link_libraries(vrx_xgstereo PRIVATE Vulkan::Headers SPIRV-Headers::SPIRV-Headers)
target_link_libraries(vrx_xgstereo PRIVATE d3d12 dxgi dxguid)
target_include_directories(vrx_xgstereo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(vrx_xgstereo PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(shader_test shader_test.cpp)
add_executable(chain_test chain_test.cpp)
target_link_libraries(chain_test PRIVATE Vulkan::Headers)
target_include_directories(chain_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(shader_test PRIVATE SPIRV-Headers::SPIRV-Headers)
if(VRX_XG_VALIDATE_SPIRV)
    target_link_libraries(shader_test PRIVATE SPIRV-Tools-static)
    target_compile_definitions(shader_test PRIVATE XG_VALIDATE_SPIRV)
endif()
add_executable(render_test render_test.cpp)
target_link_libraries(render_test PRIVATE Vulkan::Headers SPIRV-Headers::SPIRV-Headers)
target_compile_definitions(render_test PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(render_test vrx_xgstereo)
add_executable(present_test present_test.cpp)
add_executable(same_frame_test same_frame_test.cpp)
target_link_libraries(same_frame_test PRIVATE Vulkan::Headers SPIRV-Headers::SPIRV-Headers d3d12 dxgi dxguid)
target_compile_definitions(same_frame_test PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(same_frame_test vrx_xgstereo)
target_link_libraries(present_test PRIVATE Vulkan::Headers SPIRV-Headers::SPIRV-Headers user32)
target_link_libraries(present_test PRIVATE d3d12 dxgi dxguid)
target_compile_definitions(present_test PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(present_test vrx_xgstereo)
add_executable(transport_test transport_test.cpp)
target_compile_definitions(transport_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_executable(geometry_copy_test geometry_copy_test.cpp)
add_executable(gpu_transport_test gpu_transport_test.cpp)
target_link_libraries(gpu_transport_test PRIVATE d3d12 dxgi dxguid)
target_compile_definitions(gpu_transport_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(geometry_copy_test PRIVATE d3d12 dxgi dxguid)
target_compile_definitions(geometry_copy_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
foreach(target vrx_xgstereo shader_test chain_test render_test present_test same_frame_test transport_test geometry_copy_test gpu_transport_test)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /EHsc)
    endif()
endforeach()
add_custom_target(xgstereo_checks DEPENDS shader_test chain_test render_test present_test same_frame_test transport_test geometry_copy_test gpu_transport_test)
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:vrx_xgstereo>/VkLayer_vrx_xgstereo.json" CONTENT [=[
{
  "file_format_version": "1.2.0",
  "layer": {
    "name": "VK_LAYER_VRX_xgstereo",
    "type": "GLOBAL",
    "library_path": ".\\vrx_xgstereo.dll",
    "api_version": "1.4.357",
    "implementation_version": "1",
    "description": "VRX experimental geometry stereo (explicit, fixed, alternating or mirrored eyes)"
  }
}
]=])

enable_testing()
add_test(NAME isolated_runtime_startup COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime_startup_test.py"
    --layer-directory "$<TARGET_FILE_DIR:vrx_xgstereo>")
set(VRX_XG_LEGACY_RUNTIME "" CACHE FILEPATH "Optional app-local msvcp140.dll for the host runtime regression")
if(VRX_XG_LEGACY_RUNTIME)
    add_test(NAME legacy_runtime_startup COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime_startup_test.py"
        --layer-directory "$<TARGET_FILE_DIR:vrx_xgstereo>" --legacy-runtime "${VRX_XG_LEGACY_RUNTIME}")
endif()
add_test(NAME shader_patch COMMAND shader_test)
add_test(NAME device_feature_chain COMMAND chain_test)
add_test(NAME pair_transport COMMAND transport_test)
add_test(NAME gpu_transport_leases COMMAND gpu_transport_test)
set_tests_properties(gpu_transport_leases PROPERTIES TIMEOUT 30)
add_test(NAME geometry_picture_copy COMMAND geometry_copy_test)
set_tests_properties(pair_transport geometry_picture_copy PROPERTIES TIMEOUT 30)
add_test(NAME geometry_render COMMAND render_test)
add_test(NAME alternating_present COMMAND present_test)
add_test(NAME gpu_texture_present COMMAND present_test --gpu)
add_test(NAME same_frame_gpu COMMAND same_frame_test)
add_test(NAME mirror_present COMMAND present_test --mirror)
add_test(NAME mirror_inline_present COMMAND present_test --mirror-inline)
set_tests_properties(mirror_present mirror_inline_present PROPERTIES ENVIRONMENT "VK_LAYER_PATH=$<TARGET_FILE_DIR:vrx_xgstereo>" TIMEOUT 40)
set_tests_properties(same_frame_gpu PROPERTIES ENVIRONMENT "VK_LAYER_PATH=$<TARGET_FILE_DIR:vrx_xgstereo>" TIMEOUT 30)
set_tests_properties(gpu_texture_present PROPERTIES ENVIRONMENT "VK_LAYER_PATH=$<TARGET_FILE_DIR:vrx_xgstereo>" TIMEOUT 30)
add_test(NAME inline_shader_present COMMAND present_test --inline-shader)
add_test(NAME live_stereo_tuning COMMAND present_test --live-tuning)
set_tests_properties(live_stereo_tuning PROPERTIES ENVIRONMENT "VK_LAYER_PATH=$<TARGET_FILE_DIR:vrx_xgstereo>" TIMEOUT 30)
set_tests_properties(inline_shader_present PROPERTIES ENVIRONMENT "VK_LAYER_PATH=$<TARGET_FILE_DIR:vrx_xgstereo>" TIMEOUT 30)
add_test(NAME unsupported_queue_fallback COMMAND present_test --unsupported-queues)
add_test(NAME unsupported_shader_fallback COMMAND present_test --unsupported-shader)
set_tests_properties(unsupported_shader_fallback PROPERTIES ENVIRONMENT "VK_LAYER_PATH=$<TARGET_FILE_DIR:vrx_xgstereo>" TIMEOUT 30)
set_tests_properties(unsupported_queue_fallback PROPERTIES ENVIRONMENT "VK_LAYER_PATH=$<TARGET_FILE_DIR:vrx_xgstereo>" TIMEOUT 30)
set_tests_properties(alternating_present PROPERTIES ENVIRONMENT "VK_LAYER_PATH=$<TARGET_FILE_DIR:vrx_xgstereo>" TIMEOUT 30)
set_tests_properties(geometry_render PROPERTIES
    ENVIRONMENT "VK_LAYER_PATH=$<TARGET_FILE_DIR:vrx_xgstereo>"
    TIMEOUT 60)
find_program(PWSH_EXECUTABLE pwsh REQUIRED)
add_test(NAME dx11_scoped_launcher COMMAND "${PWSH_EXECUTABLE}" -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/dx11_launcher_test.ps1")
set_tests_properties(dx11_scoped_launcher PROPERTIES TIMEOUT 30)
add_test(NAME process_scoped_launcher COMMAND "${PWSH_EXECUTABLE}" -NoProfile -File
    "${CMAKE_CURRENT_SOURCE_DIR}/launcher_test.ps1" -LayerDirectory "$<TARGET_FILE_DIR:vrx_xgstereo>")
set_tests_properties(process_scoped_launcher PROPERTIES TIMEOUT 30)
set(VRX_XG_ENGINE "" CACHE FILEPATH "Built xrplayer.exe for the cross-process geometry source check")
if(VRX_XG_ENGINE)
    add_test(NAME same_frame_engine_handoff COMMAND "${PWSH_EXECUTABLE}" -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/engine_geometry_test.ps1" -Engine "${VRX_XG_ENGINE}" -Producer "$<TARGET_FILE:same_frame_test>" -Gpu -SameFrame)
    set_tests_properties(same_frame_engine_handoff PROPERTIES TIMEOUT 45)
    add_test(NAME gpu_engine_handoff COMMAND "${PWSH_EXECUTABLE}" -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/engine_geometry_test.ps1" -Engine "${VRX_XG_ENGINE}" -Producer "$<TARGET_FILE:present_test>" -Gpu)
    set_tests_properties(gpu_engine_handoff PROPERTIES TIMEOUT 45)
    add_test(NAME geometry_engine_handoff COMMAND "${PWSH_EXECUTABLE}" -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/engine_geometry_test.ps1" -Engine "${VRX_XG_ENGINE}" -Producer "$<TARGET_FILE:render_test>")
    set_tests_properties(geometry_engine_handoff PROPERTIES TIMEOUT 45)
    add_test(NAME alternating_engine_handoff COMMAND "${PWSH_EXECUTABLE}" -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/engine_geometry_test.ps1" -Engine "${VRX_XG_ENGINE}" -Producer "$<TARGET_FILE:present_test>")
    set_tests_properties(alternating_engine_handoff PROPERTIES TIMEOUT 45)
endif()

add_executable(mirror_address_test mirror_address_test.cpp)
target_link_libraries(mirror_address_test PRIVATE Vulkan::Headers SPIRV-Headers::SPIRV-Headers)
target_compile_definitions(mirror_address_test PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR WIN32_LEAN_AND_MEAN NOMINMAX)
if(VRX_XG_VALIDATE_SPIRV)
    target_link_libraries(mirror_address_test PRIVATE SPIRV-Tools-static)
    target_compile_definitions(mirror_address_test PRIVATE XG_VALIDATE_SPIRV)
endif()
if(MSVC)
    target_compile_options(mirror_address_test PRIVATE /W4 /WX /EHsc)
endif()
add_dependencies(mirror_address_test vrx_xgstereo)
add_dependencies(xgstereo_checks mirror_address_test)
add_test(NAME mirror_address_isolation COMMAND mirror_address_test)
set_tests_properties(mirror_address_isolation PROPERTIES ENVIRONMENT "VK_LAYER_PATH=$<TARGET_FILE_DIR:vrx_xgstereo>" TIMEOUT 30)

add_executable(gpu_snapshot gpu_snapshot.cpp)
target_link_libraries(gpu_snapshot PRIVATE d3d12 dxgi dxguid)
target_compile_definitions(gpu_snapshot PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(gpu_snapshot PRIVATE /W4 /WX /EHsc)
endif()

if(VRX_XG_ENGINE)
    add_test(NAME mirror_engine_handoff COMMAND "${PWSH_EXECUTABLE}" -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/engine_geometry_test.ps1" -Engine "${VRX_XG_ENGINE}" -Producer "$<TARGET_FILE:present_test>" -Mirror)
    set_tests_properties(mirror_engine_handoff PROPERTIES TIMEOUT 45)
endif()

add_test(NAME mirror_metadata_contention COMMAND present_test --mirror-contention)
set_tests_properties(mirror_metadata_contention PROPERTIES ENVIRONMENT "VK_LAYER_PATH=$<TARGET_FILE_DIR:vrx_xgstereo>" TIMEOUT 40)
