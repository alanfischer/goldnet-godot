# Standalone tests for the engine-agnostic parts of goldnet (src/goldnet_codec.h).
# No godot-cpp: its engine classes route through GDExtension function pointers that only
# exist inside a running Godot process, so anything touching Variant/StreamPeerBuffer is
# out of scope here and belongs in an in-engine test instead.
#
# Build & run:
#
#   cmake -S extern/goldnet-godot/tests -B extern/goldnet-godot/tests/build
#   cmake --build extern/goldnet-godot/tests/build
#   ctest --test-dir extern/goldnet-godot/tests/build --output-on-failure
#
cmake_minimum_required(VERSION 3.22)
project(goldnet-godot-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

# Tests rely on assert() — keep it live regardless of build type.
add_compile_options(-UNDEBUG)

# UndefinedBehaviorSanitizer, with recovery off so a violation aborts the run rather than
# printing and continuing (ctest keys on the exit code).
#
# Load-bearing, not hygiene. The angle quantizer's crash was a float→uint16 cast of NaN:
# undefined behaviour that a debug Godot build traps on but which, unsanitized, usually
# just yields 0 here — the same value the guarded path produces. Without this the
# non-finite tests in test_codec.cpp pass either way and prove nothing. Verified by
# mutation: reverting the guard fails the suite with this on, and passes with it off.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-fsanitize=undefined -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=undefined)
endif()

foreach(test_name test_codec test_seq)
    add_executable(${test_name} ${test_name}.cpp)
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
