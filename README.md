# Terminal Emulator wth OpenGL Shaders
The emualtor based off suckless st.c and inspired by Cool Retro Term.  


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

The emulator has built in OpenGL shaders which are inteded to create effets such a retro CRT look and more.


