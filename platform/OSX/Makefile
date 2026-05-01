#
# platform/OSX/Makefile — native macOS client (Cocoa + Metal + ImGui).
#
# Compiles ONLY the OSX-specific sources here (main.mm + the imgui osx
# and metal backends). Links against the core widget + emulator objects
# that the root Makefile already produced in ../../build/. Those are
# NOT rebuilt by this Makefile — if they're missing, run `make` from
# the repo root first.
#
#   Build: `make`         (from this directory)
#   Output: build/imgui_terminal_osx
#

REPO_ROOT  = ../..
IMGUI_DIR  = $(REPO_ROOT)/imgui
ROOT_BUILD = $(REPO_ROOT)/build

BUILD_DIR  = build
EXE        = $(BUILD_DIR)/imgui_terminal_osx

##---------------------------------------------------------------------
## PREREQUISITE: pre-built core objects from the root build
##---------------------------------------------------------------------
##
## ImGui core, FreeType loader, the terminal widget (imgui_win.o), and
## the st emulator core (st.o) are all platform-agnostic — once the
## root `make` has compiled them, this build just links them in.
##
## Skipping the rebuild keeps this Makefile small and makes the
## platform/OSX/ folder feel like a real native client rather than a
## second copy of the whole build graph.

ROOT_OBJS  = $(ROOT_BUILD)/imgui.o
ROOT_OBJS += $(ROOT_BUILD)/imgui_demo.o
ROOT_OBJS += $(ROOT_BUILD)/imgui_draw.o
ROOT_OBJS += $(ROOT_BUILD)/imgui_tables.o
ROOT_OBJS += $(ROOT_BUILD)/imgui_widgets.o
ROOT_OBJS += $(ROOT_BUILD)/imgui_freetype.o
ROOT_OBJS += $(ROOT_BUILD)/imgui_win.o
ROOT_OBJS += $(ROOT_BUILD)/st.o

# Parse-time existence check. Skipped for `clean` so you can always
# tidy the local build dir without the root being built.
ifeq (,$(filter clean,$(MAKECMDGOALS)))
MISSING_OBJS := $(filter-out $(wildcard $(ROOT_OBJS)),$(ROOT_OBJS))
ifneq ($(MISSING_OBJS),)
$(error Missing prerequisites in $(ROOT_BUILD)/:$(foreach o,$(MISSING_OBJS),$(\n)  - $(o))$(\n)Run `make` in the repo root first to build the core widget + emulator.)
endif
endif

##---------------------------------------------------------------------
## LOCAL SOURCES — compiled into platform/OSX/build/
##---------------------------------------------------------------------
##
## Add new client-side sources here as the native shell grows
## (window opacity, native menus, custom delegates, etc.).

LOCAL_SOURCES  = main.mm
LOCAL_SOURCES += $(IMGUI_DIR)/backends/imgui_impl_osx.mm
LOCAL_SOURCES += $(IMGUI_DIR)/backends/imgui_impl_metal.mm

LOCAL_OBJS = $(addprefix $(BUILD_DIR)/, $(addsuffix .o, $(basename $(notdir $(LOCAL_SOURCES)))))

# So the %.mm pattern rule below finds backend sources living under
# imgui/backends/.
VPATH = $(IMGUI_DIR)/backends

##---------------------------------------------------------------------
## FLAGS
##---------------------------------------------------------------------
##
## Compile flags must match the root Makefile's imgui-related flags so
## the imgui headers compile to layouts compatible with the pre-built
## .o files we link against (FreeType + WCHAR32 are toggled via
## imgui_user_config.h, found via -I$(REPO_ROOT)).

CXXFLAGS  = -std=c++11
CXXFLAGS += -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(IMGUI_DIR)/misc/freetype
CXXFLAGS += -I$(REPO_ROOT)
CXXFLAGS += -DIMGUI_USER_CONFIG=\"imgui_user_config.h\"
CXXFLAGS += $(shell pkg-config --cflags fontconfig freetype2)
CXXFLAGS += -g -Wall -Wformat

# System frameworks for Cocoa windowing + Metal rendering. GameController
# is required by imgui_impl_osx (gamepad support hook). fontconfig +
# freetype come from the widget's font discovery / rasterization path.
LIBS  = -framework Cocoa
LIBS += -framework Metal -framework MetalKit -framework QuartzCore
LIBS += -framework GameController -framework IOKit -framework CoreVideo
LIBS += -L/usr/local/lib -L/opt/homebrew/lib
LIBS += $(shell pkg-config --libs fontconfig freetype2)

##---------------------------------------------------------------------
## RULES
##---------------------------------------------------------------------

.PHONY: all clean
all: $(EXE)
	@echo "Built $(EXE)"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Single .mm pattern rule covers both local main.mm and backend .mm
# files (resolved via VPATH).
$(BUILD_DIR)/%.o: %.mm | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Link with $(CXX) so libc++ is pulled in. ROOT_OBJS land at the end
# of the link line — they reference symbols defined in LOCAL_OBJS
# (none) and vice versa (term_init/term_draw_widget/term_shutdown
# from imgui_win.o once main.mm is wired to the widget).
$(EXE): $(LOCAL_OBJS) $(ROOT_OBJS) | $(BUILD_DIR)
	$(CXX) -o $@ $(LOCAL_OBJS) $(ROOT_OBJS) $(LIBS)

clean:
	rm -rf $(BUILD_DIR)
