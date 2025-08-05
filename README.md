# Dear ImGui Terminal Emulator
The Emulator is based off suckless St.c and is implemented in C++. The project is intended to be customized and rebuilt to fit your use case. The terminal supports most ANSI standard escape codes, with good support for multiplexers and vim. The app can also be embedded in other ImGui apps such as [Ned](https://github.com/nealmick/ned).



https://github.com/user-attachments/assets/056e0a06-1188-4a7e-934c-6f998d36d7c4


# Build from source
#### Prerequisites
CMake (version 3.10 or higher)
C++17 compatible compiler
OpenGL
GLFW3
GLEW

Clone the repository with its submodules:
```sh
git clone --recursive https://github.com/nealmick/ImGui-Terminal
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

