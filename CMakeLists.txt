cmake_minimum_required(VERSION 3.24)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(GothamDuo LANGUAGES CXX ASM_MASM)

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

add_library(gothamduo SHARED
    src/dllmain.cpp
    src/dual_output.cpp
    src/dxgi_hook.cpp
    src/input_trigger.cpp
    src/version_proxy.cpp
    src/version_trampolines.asm
    src/version.def
)

set_target_properties(gothamduo PROPERTIES
    OUTPUT_NAME "version"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_compile_definitions(gothamduo PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
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
    src/input_trigger.cpp
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
