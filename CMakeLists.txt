cmake_minimum_required(VERSION 3.22)

project(goldnet-godot
    LANGUAGES CXX
    VERSION 0.1.0
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# godot-cpp, a git submodule at extern/godot-cpp. Pinned to the same commit as
# hop-godot and goldsrc-godot — keep the three in step when bumping it.
#
# This was a symlink to hop-godot's checkout, which kept a single shared copy but
# made the repo unbuildable anywhere that sibling did not exist, CI included.
add_subdirectory(extern/godot-cpp)

# Our GDExtension library
add_subdirectory(src)
