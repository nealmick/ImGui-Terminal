# Terminal Emulator with OpenGL Shaders
A crappy terminal emulator based off suckless st.c and inspired by Cool Retro Term.  
Written in C++ with less than 3k lines, the emulator is intended to be customized and rebuilt to fit your preferences. It supports most XTERM standard escape codes, with good support for multiplexers and vim. The app is built with IMGUI for performant GPU rendering and can be embedded in other apps such as [Ned](https://github.com/nealmick/ned).

https://github.com/user-attachments/assets/fec55f6a-83a5-4ff3-9c88-5fca9332ce1d

The emulator also includes OpenGL shaders which can be used for visual effects, such as old CRT look with scanlines, pixelation, static noise, vignetting, and color shift.   You must rebuild the project to update the shaders.  

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
cd terd
git submodule init
git submodule update

```

Building the Project
```sh
mkdir build
cd build
cmake ..

make

./terd
```

Contributions are welcome.

