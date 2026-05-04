#
# Cross Platform Makefile
# Compatible with MSYS2/MINGW, Ubuntu 14.04.1 and Mac OS X
#
# You will need GLFW (http://www.glfw.org):
# Linux:
#   apt-get install libglfw-dev
# Mac OS X:
#   brew install glfw
# MSYS2:
#   pacman -S --noconfirm --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-glfw
#

#CXX = g++
#CXX = clang++

BUILD_DIR = build
EXE = $(BUILD_DIR)/imgui_terminal
IMGUI_DIR = imgui

# Upstream st codebase lives in st/ as a vendored dependency. VPATH lets
# the %.o : %.c pattern rule find st.c there, and -Ist on the compile
# flags lets imgui_win.cpp's #include "st.h" resolve. Host-shell examples
# live in examples/ — same VPATH trick keeps the .o files at build/<name>.o
# regardless of source location.
VPATH = st examples tests/automation/core tests/automation/input

# C++ sources
# Widget — renderer-agnostic. NO GLFW, NO GL deps.
SOURCES  = imgui_win.cpp

# Shell — owns GLFW + OpenGL backend.
SOURCES += main_example_glfw_gl.cpp

# ImGui core
SOURCES += $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_demo.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp

# ImGui backends — only used by the shell (main.cpp), but compiled into
# the same final binary.
SOURCES += $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

# ImGui FreeType font loader (D6/§1a) — replaces stb_truetype with FreeType,
# matching x.c's rasterizer. Wired via IMGUI_ENABLE_FREETYPE in
# imgui_user_config.h.
SOURCES += $(IMGUI_DIR)/misc/freetype/imgui_freetype.cpp

# C sources — st.c is upstream st core, plain C.
C_SOURCES = st.c

CXX_OBJS = $(addprefix $(BUILD_DIR)/, $(addsuffix .o, $(basename $(notdir $(SOURCES)))))
C_OBJS   = $(addprefix $(BUILD_DIR)/, $(addsuffix .o, $(basename $(notdir $(C_SOURCES)))))
OBJS     = $(CXX_OBJS) $(C_OBJS)

# Adapter config — same convention as upstream config.def.h → config.h.
# config-imgui.h is generated from config-imgui.def.h on first build, is
# gitignored, and is user-editable. Edits persist; only the .def.h is tracked.
CONFIG_IMGUI = config-imgui.h

UNAME_S := $(shell uname -s)
LINUX_GL_LIBS = -lGL

CXXFLAGS = -std=c++11 -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(IMGUI_DIR)/misc/freetype
CXXFLAGS += -g -Wall -Wformat
# Project-level ImGui compile-time options (FreeType + WCHAR32). Keeps the
# imgui submodule's imconfig.h pristine.
CXXFLAGS += -DIMGUI_USER_CONFIG=\"imgui_user_config.h\" -I. -Ist
# Headers for fontconfig + freetype (font discovery + rasterization).
CXXFLAGS += $(shell pkg-config --cflags fontconfig freetype2)

# CFLAGS for st.c (plain C). Don't inherit -std=c++11. -I. for any root
# header st might pull in, -Ist for st.h/win.h/arg.h/config.h.
CFLAGS_C = -std=c99 -I. -Ist -g -Wall -Wformat -D_DEFAULT_SOURCE

LIBS =

##---------------------------------------------------------------------
## OPENGL ES
##---------------------------------------------------------------------

## This assumes a GL ES library available in the system, e.g. libGLESv2.so
# CXXFLAGS += -DIMGUI_IMPL_OPENGL_ES2
# LINUX_GL_LIBS = -lGLESv2

##---------------------------------------------------------------------
## BUILD FLAGS PER PLATFORM
##---------------------------------------------------------------------

ifeq ($(UNAME_S), Linux) #LINUX
	ECHO_MESSAGE = "Linux"
	LIBS += $(LINUX_GL_LIBS) `pkg-config --static --libs glfw3` -lutil
	LIBS += $(shell pkg-config --libs fontconfig freetype2)
	CXXFLAGS += `pkg-config --cflags glfw3`
endif

ifeq ($(UNAME_S), Darwin) #APPLE
	ECHO_MESSAGE = "Mac OS X"
	LIBS += -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
	LIBS += -L/usr/local/lib -L/opt/homebrew/lib
	LIBS += -lglfw
	# fontconfig + freetype for font discovery and rasterization.
	LIBS += $(shell pkg-config --libs fontconfig freetype2)
	# util.h on macOS is in the system C library; no -lutil needed.
	CXXFLAGS += -I/usr/local/include -I/opt/homebrew/include
endif

ifeq ($(OS), Windows_NT)
	ECHO_MESSAGE = "MinGW"
	LIBS += -lglfw3 -lgdi32 -lopengl32 -limm32
	LIBS += $(shell pkg-config --libs fontconfig freetype2)
	CXXFLAGS += `pkg-config --cflags glfw3`
endif

##---------------------------------------------------------------------
## BUILD RULES
##---------------------------------------------------------------------

all: $(EXE)
	@echo Build complete for $(ECHO_MESSAGE)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Generate config-imgui.h from the tracked default. User customizations
# to config-imgui.h are preserved across rebuilds (it isn't regenerated
# once it exists).
$(CONFIG_IMGUI):
	cp config-imgui.def.h $(CONFIG_IMGUI)

# imgui_win.cpp includes config-imgui.h; ensure it exists before compile.
$(BUILD_DIR)/imgui_win.o: imgui_win.cpp $(CONFIG_IMGUI) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(IMGUI_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(IMGUI_DIR)/backends/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(IMGUI_DIR)/misc/freetype/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# C compilation for st.c (plain C, not C++).
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS_C) -c -o $@ $<

# Link with $(CXX) so the C++ runtime is pulled in for ImGui.
$(EXE): $(OBJS)
	$(CXX) -o $@ $^ $(CXXFLAGS) $(LIBS)

##---------------------------------------------------------------------
## METAL TARGET — alternative renderer (macOS only)
##---------------------------------------------------------------------
##
## Same widget (imgui_win.cpp) and core (st.c) as the default OpenGL
## build; only the host shell and ImGui's renderer backend differ.
## Demonstrates the renderer-agnostic principle: identical terminal
## behavior, different GPU API underneath.
##
## Build: `make metal`  →  $(BUILD_DIR)/imgui_terminal_metal

EXE_METAL = $(BUILD_DIR)/imgui_terminal_metal

# Metal source list: the same set as SOURCES, but with the GL host shell
# swapped for example_mac_metal.mm and imgui_impl_opengl3.cpp swapped
# for imgui_impl_metal.mm. Object basenames differ, so the two builds
# share the rest of build/ without conflict.
METAL_SOURCES  = imgui_win.cpp example_mac_metal.mm
METAL_SOURCES += $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_demo.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp
METAL_SOURCES += $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp $(IMGUI_DIR)/backends/imgui_impl_metal.mm
METAL_SOURCES += $(IMGUI_DIR)/misc/freetype/imgui_freetype.cpp

METAL_OBJS = $(addprefix $(BUILD_DIR)/, $(addsuffix .o, $(basename $(notdir $(METAL_SOURCES)))))

# Metal-specific link libraries — Metal/MetalKit/QuartzCore replace
# OpenGL. Cocoa/IOKit/CoreVideo + GLFW + fontconfig/freetype are
# common to both renderers but listed here explicitly since the LIBS
# variable above is OpenGL-targeted.
LIBS_METAL  = -framework Metal -framework MetalKit -framework QuartzCore
LIBS_METAL += -framework Cocoa -framework IOKit -framework CoreVideo
LIBS_METAL += -L/usr/local/lib -L/opt/homebrew/lib -lglfw
LIBS_METAL += $(shell pkg-config --libs fontconfig freetype2)

# .mm compilation. clang treats .mm as Objective-C++ by extension; the
# rest of CXXFLAGS works as-is.
$(BUILD_DIR)/%.o: %.mm | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -fobjc-arc -Wno-deprecated-declarations -c -o $@ $<

$(BUILD_DIR)/main.o: platform/OSX/main.mm | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -fobjc-arc -Wno-deprecated-declarations -c -o $@ $<

$(BUILD_DIR)/imgui_impl_osx.o: $(IMGUI_DIR)/backends/imgui_impl_osx.mm | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -fobjc-arc -Wno-deprecated-declarations -c -o $@ $<

$(BUILD_DIR)/imgui_impl_metal.o: $(IMGUI_DIR)/backends/imgui_impl_metal.mm | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -fobjc-arc -Wno-deprecated-declarations -c -o $@ $<

$(BUILD_DIR)/%.o: $(IMGUI_DIR)/backends/%.mm | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -fobjc-arc -Wno-deprecated-declarations -c -o $@ $<

.PHONY: metal
metal: $(EXE_METAL)
	@echo Metal build complete

$(EXE_METAL): $(METAL_OBJS) $(C_OBJS)
	$(CXX) -o $@ $^ $(CXXFLAGS) $(LIBS_METAL)

##---------------------------------------------------------------------
## VULKAN TARGET — alternative renderer (cross-platform)
##---------------------------------------------------------------------
##
## Requires the Vulkan SDK installed:
##   macOS: brew install vulkan-headers vulkan-loader molten-vk
##          (or LunarG VulkanSDK)
##   Linux: apt install libvulkan-dev (Debian/Ubuntu) or distro equivalent
##
## Build: `make vulkan`  →  $(BUILD_DIR)/imgui_terminal_vulkan

EXE_VULKAN = $(BUILD_DIR)/imgui_terminal_vulkan

VULKAN_SOURCES  = imgui_win.cpp example_glfw_vulkan.cpp
VULKAN_SOURCES += $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_demo.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp
VULKAN_SOURCES += $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp $(IMGUI_DIR)/backends/imgui_impl_vulkan.cpp
VULKAN_SOURCES += $(IMGUI_DIR)/misc/freetype/imgui_freetype.cpp

VULKAN_OBJS = $(addprefix $(BUILD_DIR)/, $(addsuffix .o, $(basename $(notdir $(VULKAN_SOURCES)))))

# Vulkan link libraries — cross-platform via pkg-config. Plus the
# common GLFW + fontconfig + freetype the widget needs.
ifneq (,$(filter vulkan,$(MAKECMDGOALS)))
LIBS_VULKAN  = $(shell pkg-config --libs vulkan)
LIBS_VULKAN += -L/usr/local/lib -L/opt/homebrew/lib -lglfw
LIBS_VULKAN += $(shell pkg-config --libs fontconfig freetype2)

# rpath so the binary finds libvulkan at runtime — pkg-config gives
# us the lib dir; embed it as an rpath so dyld/ld.so can resolve
# libvulkan.1.dylib (or libvulkan.so) without LD_LIBRARY_PATH /
# DYLD_LIBRARY_PATH gymnastics at run time.
VULKAN_LIBDIR = $(shell pkg-config --variable=libdir vulkan)
LIBS_VULKAN += -Wl,-rpath,$(VULKAN_LIBDIR)
endif

ifeq ($(UNAME_S), Darwin)
	LIBS_VULKAN += -framework Cocoa -framework IOKit -framework CoreVideo
endif
ifeq ($(UNAME_S), Linux)
	LIBS_VULKAN += -lutil
endif

.PHONY: vulkan
vulkan: $(EXE_VULKAN)
	@echo Vulkan build complete

$(EXE_VULKAN): $(VULKAN_OBJS) $(C_OBJS)
	$(CXX) -o $@ $^ $(CXXFLAGS) $(LIBS_VULKAN)

##---------------------------------------------------------------------
## TEST CLIENTS — headless harnesses for tests/automation/
##---------------------------------------------------------------------
##
## Same imgui_win.cpp + st.c as the shipping binaries. No GLFW, no GPU
## backend. Two flavors:
##
##   test_core  — scripted bash child writes to PTY; dump after EOF.
##                Tests render pipeline (xdrawline, color resolution,
##                attribute pipeline, glyph emission). No frame loop.
##                Build: `make test_core`  →  $(BUILD_DIR)/test_core
##
##   test_input — bash -i; events.txt drives ImGui keyboard/mouse
##                events between real frames. Tests the input pipeline
##                end-to-end (ImGui event → dispatcher → ttywrite →
##                bash → ttyread → row cache).
##                Build: `make test_input` →  $(BUILD_DIR)/test_input

# Shared sources (imgui core + freetype loader). The harness-specific
# main lives in TEST_CORE_SRC / TEST_INPUT_SRC below.
TEST_SHARED_SOURCES  = imgui_win.cpp
TEST_SHARED_SOURCES += $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_demo.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp
TEST_SHARED_SOURCES += $(IMGUI_DIR)/misc/freetype/imgui_freetype.cpp

TEST_SHARED_OBJS = $(addprefix $(BUILD_DIR)/, $(addsuffix .o, $(basename $(notdir $(TEST_SHARED_SOURCES)))))

# No GLFW, no OpenGL/Metal/Vulkan. fontconfig+freetype are required because
# imgui_win.cpp's font loading uses them.
LIBS_TEST = $(shell pkg-config --libs fontconfig freetype2)

EXE_TEST_CORE  = $(BUILD_DIR)/test_core
EXE_TEST_INPUT = $(BUILD_DIR)/test_input

.PHONY: test_core test_input
test_core:  $(EXE_TEST_CORE)
	@echo "Test client core build complete"
test_input: $(EXE_TEST_INPUT)
	@echo "Test client input build complete"

$(EXE_TEST_CORE): $(BUILD_DIR)/test_core.o  $(TEST_SHARED_OBJS) $(C_OBJS)
	$(CXX) -o $@ $^ $(CXXFLAGS) $(LIBS_TEST)

$(EXE_TEST_INPUT): $(BUILD_DIR)/test_input.o $(TEST_SHARED_OBJS) $(C_OBJS)
	$(CXX) -o $@ $^ $(CXXFLAGS) $(LIBS_TEST)

##---------------------------------------------------------------------
## NATIVE OSX CLIENT — Cocoa + Metal
##---------------------------------------------------------------------
##
## Same widget (imgui_win.cpp) and core (st.c) as the other targets.
## Replaces GLFW with native Cocoa windowing and Metal rendering.
##
## Build: `make p_osx` →  $(BUILD_DIR)/p_osx

EXE_P_OSX = $(BUILD_DIR)/p_osx

P_OSX_SOURCES  = imgui_win.cpp platform/OSX/main.mm
P_OSX_SOURCES += $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_demo.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp
P_OSX_SOURCES += $(IMGUI_DIR)/backends/imgui_impl_osx.mm $(IMGUI_DIR)/backends/imgui_impl_metal.mm
P_OSX_SOURCES += $(IMGUI_DIR)/misc/freetype/imgui_freetype.cpp

P_OSX_OBJS = $(addprefix $(BUILD_DIR)/, $(addsuffix .o, $(basename $(notdir $(P_OSX_SOURCES)))))

# Native Cocoa/Metal frameworks. GameController is required by
# imgui_impl_osx.mm. fontconfig+freetype for the widget's font path.
LIBS_P_OSX  = -framework Cocoa -framework Metal -framework MetalKit
LIBS_P_OSX += -framework QuartzCore -framework GameController
LIBS_P_OSX += -framework IOKit -framework CoreVideo
LIBS_P_OSX += -L/usr/local/lib -L/opt/homebrew/lib
LIBS_P_OSX += $(shell pkg-config --libs fontconfig freetype2)

.PHONY: p_osx
p_osx: $(EXE_P_OSX)
	@echo Native OSX build complete

$(EXE_P_OSX): $(P_OSX_OBJS) $(C_OBJS)
	$(CXX) -o $@ $^ $(CXXFLAGS) $(LIBS_P_OSX)

clean:
	rm -rf $(BUILD_DIR)

# distclean: also remove generated config-imgui.h. Use this when you want
# a fully pristine state (e.g., to pick up new defaults from the .def.h).
distclean: clean
	rm -f $(CONFIG_IMGUI)
