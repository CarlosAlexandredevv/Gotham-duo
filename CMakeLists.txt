cmake_minimum_required(VERSION 3.24)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(GOTHAMDUO_VERSION "0.1.0" CACHE STRING "Gotham Duo version (MAJOR.MINOR.PATCH[-prerelease])")
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _gothamduo_ver_match "${GOTHAMDUO_VERSION}")
if(NOT _gothamduo_ver_match)
    message(FATAL_ERROR "GOTHAMDUO_VERSION must start with MAJOR.MINOR.PATCH, got: ${GOTHAMDUO_VERSION}")
endif()
set(GOTHAMDUO_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(GOTHAMDUO_VERSION_MINOR "${CMAKE_MATCH_2}")
set(GOTHAMDUO_VERSION_PATCH "${CMAKE_MATCH_3}")

project(GothamDuo
    VERSION "${GOTHAMDUO_VERSION_MAJOR}.${GOTHAMDUO_VERSION_MINOR}.${GOTHAMDUO_VERSION_PATCH}"
    LANGUAGES CXX ASM_MASM
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Gotham Duo requires an x64 target.")
endif()

include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
)
FetchContent_MakeAvailable(minhook)

configure_file(
    src/proxy/version.rc.in
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    @ONLY
)

add_library(gothamduo SHARED
    src/hooks/d3d12_probe.cpp
    src/hooks/dxgi_hook.cpp
    src/input/input_trigger.cpp
    src/output/dual_output.cpp
    src/output/split_seam_detector.cpp
    src/proxy/dllmain.cpp
    src/proxy/version_proxy.cpp
    src/proxy/version_trampolines.asm
    src/proxy/version.def
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
)

set_target_properties(gothamduo PROPERTIES
    OUTPUT_NAME "version"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_include_directories(gothamduo PRIVATE
    src
)

target_compile_definitions(gothamduo PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    GOTHAMDUO_VERSION_STR="${GOTHAMDUO_VERSION}"
)

target_link_libraries(gothamduo PRIVATE
    minhook
    d3d12
    dxgi
    user32
)

enable_testing()

add_executable(gothamduo_tests
    tests/input_trigger_test.cpp
    src/input/input_trigger.cpp
)

set_target_properties(gothamduo_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_include_directories(gothamduo_tests PRIVATE
    src
)

target_compile_definitions(gothamduo_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

target_link_libraries(gothamduo_tests PRIVATE
    user32
)

add_test(NAME input_trigger_tests COMMAND gothamduo_tests)

add_executable(duo_resize_guard_tests
    tests/duo_resize_guard_test.cpp
)

set_target_properties(duo_resize_guard_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_include_directories(duo_resize_guard_tests PRIVATE
    src
)

add_test(NAME duo_resize_guard_tests COMMAND duo_resize_guard_tests)

add_executable(split_seam_detector_tests
    tests/split_seam_detector_test.cpp
    src/output/split_seam_detector.cpp
)

set_target_properties(split_seam_detector_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_include_directories(split_seam_detector_tests PRIVATE
    src
)

target_compile_definitions(split_seam_detector_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

add_test(NAME split_seam_detector_tests COMMAND split_seam_detector_tests)

add_executable(output_visibility_tests
    tests/output_visibility_test.cpp
)

set_target_properties(output_visibility_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_include_directories(output_visibility_tests PRIVATE
    src
)

add_test(NAME output_visibility_tests COMMAND output_visibility_tests)

add_executable(config_tests
    tests/config_test.cpp
)

set_target_properties(config_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_include_directories(config_tests PRIVATE
    src
)

add_test(NAME config_tests COMMAND config_tests)

add_executable(monitor_layout_tests
    tests/monitor_layout_test.cpp
)

set_target_properties(monitor_layout_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_include_directories(monitor_layout_tests PRIVATE
    src
)

add_test(NAME monitor_layout_tests COMMAND monitor_layout_tests)

add_executable(present_policy_tests
    tests/present_policy_test.cpp
)

set_target_properties(present_policy_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_include_directories(present_policy_tests PRIVATE
    src
)

add_test(NAME present_policy_tests COMMAND present_policy_tests)
