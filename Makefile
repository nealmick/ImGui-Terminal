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
# flags lets imgui_win.cpp's #include "st.h" resolve.
VPATH = st

# C++ sources
# Widget — renderer-agnostic. NO GLFW, NO GL deps.
SOURCES  = imgui_win.cpp

# Shell — owns GLFW + OpenGL backend.
SOURCES += main.cpp

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

clean:
	rm -rf $(BUILD_DIR)

# distclean: also remove generated config-imgui.h. Use this when you want
# a fully pristine state (e.g., to pick up new defaults from the .def.h).
distclean: clean
	rm -f $(CONFIG_IMGUI)
