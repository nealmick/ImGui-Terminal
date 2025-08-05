cmake_minimum_required(VERSION 3.10)
project(terminal)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)

# Try to find GLEW using pkg-config first, then fall back to manual paths
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GLEW QUIET glew)
endif()

if(NOT GLEW_FOUND)
    # Fall back to manual GLEW detection
    # Try common Homebrew paths
    set(GLEW_POSSIBLE_PATHS
        "/opt/homebrew/opt/glew"
        "/usr/local/opt/glew"
        "/usr/local/Cellar/glew"
    )
    
    foreach(GLEW_PATH ${GLEW_POSSIBLE_PATHS})
        if(EXISTS "${GLEW_PATH}/lib/libGLEW.dylib")
            set(GLEW_INCLUDE_DIRS "${GLEW_PATH}/include")
            set(GLEW_LIBRARY_DIRS "${GLEW_PATH}/lib")
            set(GLEW_LIBRARIES "${GLEW_PATH}/lib/libGLEW.dylib")
            set(GLEW_FOUND TRUE)
            break()
        endif()
    endforeach()
    
    if(NOT GLEW_FOUND)
        message(FATAL_ERROR "GLEW not found. Please install it with: brew install glew")
    endif()
endif()

# Copy resources (only fonts now, no shaders)
file(COPY ${CMAKE_SOURCE_DIR}/fonts DESTINATION ${CMAKE_BINARY_DIR})

# Main executable
add_executable(terminal
    main.cpp
    terminal.cpp   
    imgui/imgui.cpp
    imgui/imgui_demo.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_glfw.cpp
    imgui/backends/imgui_impl_opengl3.cpp
)

target_include_directories(terminal PRIVATE 
    imgui
    imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}
    ${GLEW_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(terminal PRIVATE 
    ${OPENGL_LIBRARIES}
    glfw
    ${GLEW_LIBRARIES}
)

# Add library directories for GLEW
if(GLEW_LIBRARY_DIRS)
    target_link_directories(terminal PRIVATE ${GLEW_LIBRARY_DIRS})
endif()

if(APPLE)
    target_link_libraries(terminal PRIVATE 
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreVideo"
        "-framework OpenGL"
    )
endif()
