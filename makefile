# --- Compiler and Project Name ---
CC = gcc
TARGET = spectravisual

# --- Source and Object Files ---
SRCS = main.c view.c controller.c algorithms.c layout.c loader.c
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
CFLAGS = -Wall -Wextra -Wpedantic -g

# --- Library Flags ---
# -lm: Math library
LDFLAGS = -lm

# --- SDL2 Configuration (Auto-detect) ---
# This works if sdl2-config is in your PATH. 
# It usually comes with SDL2 installation.
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LDFLAGS := $(shell sdl2-config --libs) -lSDL2_ttf

# The application is ad-hoc signed only on macOS.  A Linux CI build must not
# depend on the macOS-only `codesign` command.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SIGN_BINARY = codesign --force --sign - $(TARGET)
else
SIGN_BINARY = @true
endif

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
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(SDL_LDFLAGS) $(LDFLAGS)
	$(SIGN_BINARY)
	@echo "Build successful! Run with: ./$(TARGET) exp.csv pred.cat"

# Compile source files into object files
# This generic rule works for all .c files in the list
%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

# Clean up build files
clean:
	rm -f $(OBJS) $(TARGET) $(TEST_TARGET)

# Helper to run the app quickly (adjust arguments as needed)
run: $(TARGET)
	./$(TARGET) exp.csv pred.cat

# Fast deterministic checks for the scientific core.  These tests deliberately
# do not create files in the source root or require a graphics display.
TEST_TARGET = tests/test_core
TEST_SRCS = tests/test_core.c algorithms.c loader.c

$(TEST_TARGET): $(TEST_SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -I. $(TEST_SRCS) -o $@ -lm

test: $(TEST_TARGET)
	./$(TEST_TARGET)

check: all test

# Deploy the latest build to the parent liveplot/ (the PATH copy) and RE-SIGN it
# there. A plain `cp` invalidates the ad-hoc signature -> macOS SIGKILLs it.
# Use `make deploy` instead of copying by hand.
deploy: $(TARGET)
	cp $(TARGET) ../$(TARGET)
	codesign --force --sign - ../$(TARGET)
	@echo "Deployed + signed: ../$(TARGET)"

.PHONY: all clean run test check deploy
