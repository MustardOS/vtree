# vTree Makefile
# Builds for the host by default; cross-compile by setting CC and SYSROOT.
#
# Usage:
#   make                  — debug build (sanitizers, symbols)
#   make release          — optimised release build
#   make clean            — remove build artefacts
#
# Cross-compile example (muOS / ARM):
#   make CC=arm-linux-gnueabihf-gcc release

CC      ?= gcc
TARGET  := vtree

SRCS    := main.c config.c fileop.c viewer.c hexview.c imgview.c keyboard.c snake.c lang.c ui_audio.c
OBJS    := $(SRCS:.c=.o)

SDL2_CFLAGS :=
SDL2_LIBS   := -lSDL2

CFLAGS_COMMON := -Wall -Wextra -std=c99 $(SDL2_CFLAGS) \
                 -DSDL_MAIN_HANDLED \
                 -D_POSIX_C_SOURCE=200809L

LIBS := $(SDL2_LIBS) -lSDL2_ttf -lSDL2_image -lm

# Debug (default)
CFLAGS  := $(CFLAGS_COMMON) -g -O0 -fsanitize=address,undefined
LDFLAGS := -fsanitize=address,undefined

# Release
CFLAGS_REL  := $(CFLAGS_COMMON) -O2 -DNDEBUG
LDFLAGS_REL :=

LIBS    := $(SDL2_LIBS) -lSDL2_ttf -lSDL2_image -lm

.PHONY: all release clean

all: $(TARGET)

release: CFLAGS  = $(CFLAGS_REL)
release: LDFLAGS = $(LDFLAGS_REL)
release: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c vtree.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)