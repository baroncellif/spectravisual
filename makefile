# --- Compiler and Project Name ---
CC = gcc
TARGET = spectravisual

# --- Source and Object Files ---
SRCS = main.c view.c controller.c algorithms.c layout.c loader.c intensity_fit.c predfit.c settings.c ui_icons.c

# Dear ImGui draws the spectrum lines; ImPlot is compiled in and ready for the
# panes themselves, which still use the application's own axes and decimation.
IMGUI_DIR  = third_party/imgui
IMPLOT_DIR = third_party/implot
CXXSRCS = plotgpu.cpp \
          $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp \
          $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp \
          $(IMGUI_DIR)/backends/imgui_impl_sdlrenderer2.cpp \
          $(IMPLOT_DIR)/implot.cpp $(IMPLOT_DIR)/implot_items.cpp
CXXOBJS = $(CXXSRCS:.cpp=.o)
CXX = c++
CXXFLAGS = -O2 -g -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(IMPLOT_DIR)
OBJS = $(SRCS:.c=.o)

# --- Header dependencies ---
# Every object depends on all project headers. Without this, editing a header
# (e.g. changing the AppState struct in types.h) would NOT trigger a recompile
# of the .c files that weren't themselves edited, leaving object files with
# mismatched struct layouts -> memory corruption / crashes. Keep it simple and
# safe: rebuild every object whenever any header changes.
HDRS = $(wildcard *.h)

# --- Compiler Flags ---
# -Wall: Enable all warnings
# -g: Add debug info (useful for lldb/gdb)
CFLAGS = -Wall -g

# --- Library Flags ---
# -lm: Math library
LDFLAGS = -lm

# --- SDL2 Configuration (Auto-detect) ---
# This works if sdl2-config is in your PATH. 
# It usually comes with SDL2 installation.
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LDFLAGS := $(shell sdl2-config --libs) -lSDL2_ttf

# --- SDL2 Configuration (Manual macOS Fallback) ---
# If the auto-detect above fails, comment out the two lines above 
# and uncomment the lines below based on your architecture.

# 1. For Apple Silicon (M1/M2/M3) using Homebrew:
# SDL_CFLAGS  = -I/opt/homebrew/include/SDL2 -D_THREAD_SAFE
# SDL_LDFLAGS = -L/opt/homebrew/lib -lSDL2 -lSDL2_ttf

# 2. For Intel Mac using Homebrew:
# SDL_CFLAGS  = -I/usr/local/include/SDL2 -D_THREAD_SAFE
# SDL_LDFLAGS = -L/usr/local/lib -lSDL2 -lSDL2_ttf

# --- Build Rules ---

# Default rule: Build the executable
all: $(TARGET)

# Link the object files into the executable
$(TARGET): $(OBJS) $(CXXOBJS)
	$(CXX) $(OBJS) $(CXXOBJS) -o $(TARGET) $(SDL_LDFLAGS) $(LDFLAGS)
	codesign --force --sign - $(TARGET)
	@echo "Build successful! Run with: ./$(TARGET) exp.csv pred.cat"

# Compile source files into object files
# This generic rule works for all .c files in the list
%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

%.o: %.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -c $< -o $@

# --- Regression suite (docs/audit/PIANO-FIX.md) ---
# tests/test_audit.c #includes main.c, controller.c and predfit.c, so it is
# linked against every other source except the ImGui renderer, which the tests
# never use (tests/plotgpu_stub.c).  `make test` builds and runs all of it;
# `./tests/test_audit <name>` runs a single test.
TEST_BIN  = tests/test_audit
TEST_SRCS = loader.c algorithms.c layout.c settings.c ui_icons.c intensity_fit.c view.c tests/plotgpu_stub.c

$(TEST_BIN): tests/test_audit.c main.c controller.c predfit.c $(TEST_SRCS) $(HDRS)
	$(CC) -g -O0 -Wall -Wno-unused-function $(SDL_CFLAGS) -I. -DSV_TESTS_DIR='"$(CURDIR)/tests"' \
	    -o $@ tests/test_audit.c $(TEST_SRCS) $(SDL_LDFLAGS) $(LDFLAGS)

test: $(TEST_BIN)
	./$(TEST_BIN)

# Clean up build files
clean:
	rm -f $(OBJS) $(TARGET) $(TEST_BIN)
	rm -rf $(TEST_BIN).dSYM

# Helper to run the app quickly (adjust arguments as needed)
run: $(TARGET)
	./$(TARGET) exp.csv pred.cat

# There is no deploy step: the binary on the PATH is ./spectravisual in this
# folder, and the link rule above already signs it.

.PHONY: all clean run test
