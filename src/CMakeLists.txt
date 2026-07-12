project(goldnet)

# Collect all source files
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

# Shared library for the GDExtension
add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    # The engine-agnostic goldnet core lands here in Phase 6 (mirrors extern/hop/include).
    ${CMAKE_SOURCE_DIR}/extern/goldnet/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE godot::cpp)

# Force-load the entire godot-cpp archive. godot-cpp registers each engine class's
# instance-binding callbacks from a static initializer in that class's own object
# file, and the linker normally drops object files whose symbols we never
# reference. A MultiplayerAPI implementation is handed *arbitrary* engine objects
# through its virtuals (_rpc's target node, the synchronizers/spawners in
# _object_configuration_add — AnimatableBody3D, Node3D, …), and godot-cpp must be
# able to wrap any of them. Without whole-archive linking those registrations are
# stripped and wrapping fails with "Cannot find instance binding callbacks for
# class '…'". Unlike a physics/compute extension (hop), we can't enumerate the set
# ahead of time, so pull them all in.
if(APPLE)
    target_link_options(${PROJECT_NAME} PRIVATE "-Wl,-force_load,$<TARGET_FILE:godot::cpp>")
else()
    target_link_options(${PROJECT_NAME} PRIVATE
        "-Wl,--whole-archive" "$<TARGET_FILE:godot::cpp>" "-Wl,--no-whole-archive")
endif()

# Output naming: libgoldnet.<platform>.template_<config>.<arch>
string(TOLOWER "${CMAKE_SYSTEM_NAME}" SYSTEM_NAME)
if(SYSTEM_NAME STREQUAL "darwin")
    set(SYSTEM_NAME "macos")
endif()
# Allow toolchain files to override platform name (e.g. "android" when CMAKE_SYSTEM_NAME=Linux)
if(GOLDNET_PLATFORM_NAME)
    set(SYSTEM_NAME "${GOLDNET_PLATFORM_NAME}")
endif()
string(TOLOWER "${CMAKE_BUILD_TYPE}" BUILD_TYPE)

if(BUILD_TYPE STREQUAL "")
    set(BUILD_TYPE "debug")
endif()

# Map cmake build type to godot convention
if(BUILD_TYPE STREQUAL "debug" OR BUILD_TYPE STREQUAL "relwithdebinfo")
    set(GODOT_BUILD_TYPE "template_debug")
else()
    set(GODOT_BUILD_TYPE "template_release")
endif()

# Determine architecture string
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    set(ARCH "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "AMD64")
    set(ARCH "x86_64")
else()
    set(ARCH ${CMAKE_SYSTEM_PROCESSOR})
endif()

set(OUTPUT_NAME "goldnet.${SYSTEM_NAME}.${GODOT_BUILD_TYPE}.${ARCH}")

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "${OUTPUT_NAME}"
    PREFIX "lib"
)

# Output to the addon's bin directory
set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/addons/goldnet/bin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/addons/goldnet/bin"
)
