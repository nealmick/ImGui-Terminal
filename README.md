<img width="150" height="150" alt="logo-2" src="https://github.com/user-attachments/assets/a07ad730-add1-480a-a2b1-7cf968bc4737" />

# ImGui Terminal Emulator

An ImGui-native terminal emulator based on [suckless st][st]. Replaces st's X11/Xft frontend (`x.c`) with an ImGui-based adapter that runs renderer agnostic — GLFW+OpenGL, Vulkan, Metal, or any other ImGui backend.

**Cross-renderer portability.** st is tied to X11. wherever ImGui runs this terminal runs.  
**Composable as a widget.** The terminal is exposed as `Terminal::init` /
`Terminal::draw_widget` / `Terminal::shutdown`. For direct control,
`Terminal::draw_canvas` renders into the current ImGui window.

## Build

CMake 3.21+ and a **C++20 compiler** (gcc 10+, clang 10+, Apple clang
12+ / Xcode 12+, MSVC 19.29+ / VS 2019 16.10+). Requires fontconfig
and freetype.

### Windows (MSVC)

Visual Studio 2019+. Clone vcpkg into the repo once and bootstrap it;
CMake auto-detects it from `./vcpkg/`:

```powershell
git clone https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat
cmake -B build -S .
cmake --build build --config Release
.\build\imgui_terminal.exe
```

### Windows (MinGW / MSYS2)
From a MinGW64 shell (mintty):

```sh
pacman -S make cmake mingw-w64-x86_64-toolchain \
          mingw-w64-x86_64-glfw mingw-w64-x86_64-freetype \
          mingw-w64-x86_64-fontconfig mingw-w64-x86_64-pkg-config
cmake -B build -S .
cmake --build build
./build/imgui_terminal
```

### Linux / macOS

Install deps via your package manager (apt, brew, etc.), then:

```sh
cmake -B build -S .
cmake --build build
./build/imgui_terminal
```

On macOS, launch with `open ./build/imgui_terminal` direct exec from
terminal can't grab keyboard focus on macOS 26 + GLFW 3.4.


### Alternative renderers / native clients

```sh
cmake --build build --target imgui_terminal_metal   # macOS only
cmake --build build --target imgui_terminal_vulkan  # needs Vulkan SDK
cmake --build build --target p_osx                  # native macOS Cocoa+Metal
```


## What works

- Terminal emulation — bash, zsh, fish, vim, tmux, htop, less, and more. and more...
- Keyboard: input dispatch shortcuts, kmap, chords
- Mouse: drag-select, double/triple-click word/line snap, wheel, custom
  bindings, **plus mouse reporting** (X10/VT200/SGR) — vim/tmux/htop
  mouse modes work!
- Fonts: 4 variants (regular/bold/italic/bold-italic) via fontconfig
  discovery, **plus CJK and color emoji fallback** (Hiragino/Noto +
  Apple/Noto Color Emoji)
- Colors: 256-color palette, truecolor, X11 `rgb.txt` color names,
  runtime palette mutation via OSC sequences
- HiDPI: density-aware rasterization on Retina displays
- Focus events: `\033[I` / `\033[O` reporting for tmux focus-events,
  vim autoread, etc.
- Selection + clipboard: drag-to-select, OS clipboard via ImGui's API,
  bracketed paste



## The idea - ImGui plays the role of X11
The widget and emulator is implemented in the terminal class and deliberately uses ImGui APIs only — no GLFW, OpenGL, or
Vulkan.

```cpp
#include "terminal.h"      // link terminal.cpp

Terminal t;
t.init(80, 24, nullptr);   // spawn the system shell
// once per ImGui frame:
t.draw_widget("###term");
// on teardown:
t.shutdown();
```


#### Demo
https://github.com/user-attachments/assets/b1fafe54-587e-40de-b826-1904cfdf93cd


#### Windows
<img width="1796" height="1084" alt="Screenshot 2026-05-27 at 12 41 51 PM" src="https://github.com/user-attachments/assets/f95eef04-0344-4f53-bda0-9291c4e9222b" />

## License
- This code (root, including `terminal.h`, `terminal.cpp`,
  `CMakeLists.txt`, build config, etc.) — see `LICENSE` (BSL 1.1), me (at) nealmick (dot) com for licensing 
- Upstream st (`st/`) — see `st/LICENSE` (MIT)
- ImGui (`imgui/`) — see `imgui/LICENSE.txt` (MIT)

[st]: https://st.suckless.org/

