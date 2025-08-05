# Terminal Emulator with OpenGL Shaders 
Written in C++ with less than 3k lines, the emulator is intended to be customized and rebuilt to fit your preferences. It supports most XTERM standard escape codes, with good support for multiplexers and vim. The app is built with IMGUI for performant GPU rendering and can be embedded in other apps such as [Ned](https://github.com/nealmick/ned).


# Build from source
#### Prerequisites
CMake (version 3.10 or higher)
C++17 compatible compiler
OpenGL
GLFW3
GLEW

Clone the repository with its submodules:
```sh
git clone --recursive https://github.com/nealmick/terd
cd ImGui-terminal
git submodule init
git submodule update

```

Building the Project
```sh
mkdir build
cd build
cmake ..

make

./terminal
```

Contributions are welcome.

