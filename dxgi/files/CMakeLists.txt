cmake_minimum_required(VERSION 3.16)
project(dxgi-mirror LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release by default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Optimize for speed in Release
set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")

add_executable(dxgi-mirror WIN32 main.cpp)

# Link required libraries
target_link_libraries(dxgi-mirror PRIVATE
    d3d11
    dxgi
    d3dcompiler
    user32
)

# Console subsystem (we want console output)
set_target_properties(dxgi-mirror PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:CONSOLE"
)
