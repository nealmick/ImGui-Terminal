cmake_minimum_required(VERSION 3.10)
project(terd)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)

# Explicitly set GLEW paths
set(GLEW_INCLUDE_DIRS "/usr/local/Cellar/glew/2.2.0_1/include")
set(GLEW_LIBRARY_DIRS "/usr/local/Cellar/glew/2.2.0_1/lib")
set(GLEW_LIBRARIES "/usr/local/Cellar/glew/2.2.0_1/lib/libGLEW.dylib")



# Copy resources
file(COPY ${CMAKE_SOURCE_DIR}/fonts DESTINATION ${CMAKE_BINARY_DIR})
file(COPY ${CMAKE_SOURCE_DIR}/shaders DESTINATION ${CMAKE_BINARY_DIR})



# Main executable
add_executable(terd
    main.cpp
    shaders/shader.cpp
    terminal.cpp   
    imgui/imgui.cpp
    imgui/imgui_demo.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_glfw.cpp
    imgui/backends/imgui_impl_opengl3.cpp
)
target_include_directories(terd PRIVATE 
    imgui
    imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}
    ${GLEW_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(terd PRIVATE 
    ${OPENGL_LIBRARIES}
    glfw
    ${GLEW_LIBRARIES}
)

if(APPLE)
    target_link_libraries(terd PRIVATE 
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreVideo"
        "-framework OpenGL"
    )
endif()
