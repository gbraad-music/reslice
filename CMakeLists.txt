cmake_minimum_required(VERSION 3.15)
project(regroove VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add /usr/local to search paths
set(CMAKE_PREFIX_PATH "/usr/local;${CMAKE_PREFIX_PATH}")
list(APPEND CMAKE_LIBRARY_PATH /usr/local/lib)
list(APPEND CMAKE_INCLUDE_PATH /usr/local/include)

# Set pkg-config to also search /usr/local
set(ENV{PKG_CONFIG_PATH} "/usr/local/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")

# Find required packages
find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL2 REQUIRED sdl2 aubio)

# GUI version
add_executable(reslice-gui
    reslice-gui.cpp
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_sdl2.cpp
    imgui/backends/imgui_impl_opengl2.cpp
)

target_include_directories(reslice-gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    imgui
    imgui/backends
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(reslice-gui PRIVATE
    ${SDL2_LIBRARIES}
    ${AUBIO_LIBRARIES}
)

# Platform-specific libraries
if(WIN32)
    target_link_libraries(reslice-gui PRIVATE
        opengl32
        imm32
        version
        setupapi
        z
    )
else()
    target_link_libraries(reslice-gui PRIVATE
        GL
        dl
        pthread
        m
    )
endif()

target_compile_options(reslice-gui PRIVATE
    ${SDL2_CFLAGS_OTHER}
)

# Installation rules
install(TARGETS reslice-gui
    RUNTIME DESTINATION bin
)
