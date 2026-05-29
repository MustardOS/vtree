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
                 -D_POSIX_C_SOURCE=200809L \
                 -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
# _FILE_OFFSET_BITS=64 makes off_t/stat/fseeko/ftello 64-bit on 32-bit targets
# (armhf): required for >2 GB files and so copy_file_range's loff_t* offsets
# match off_t. No-op on 64-bit. _LARGEFILE_SOURCE exposes fseeko/ftello.

LIBS := $(SDL2_LIBS) -lSDL2_ttf -lSDL2_image -lm -lpthread

# Debug (default)
CFLAGS  := $(CFLAGS_COMMON) -g -O0 -fsanitize=address,undefined
LDFLAGS := -fsanitize=address,undefined

# Release — with runtime hardening (FORTIFY needs -O1+; stack canaries).
CFLAGS_REL  := $(CFLAGS_COMMON) -O2 -DNDEBUG \
               -D_FORTIFY_SOURCE=2 -fstack-protector-strong
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