# Debug build by default: this is a teaching codebase, so we want ASan and
# full debug info while poking at it. `make BUILD=release` for speed.
BUILD ?= debug

CC      := gcc
CFLAGS  := -std=c99 -Wall -Wextra $(shell sdl2-config --cflags)
LDFLAGS := $(shell sdl2-config --libs)

ifeq ($(BUILD),debug)
CFLAGS  += -g -fsanitize=address
LDFLAGS += -fsanitize=address
else
CFLAGS  += -O2
endif

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

gbemu: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

# Every .c depends on every header: the project is small enough that precise
# dependency tracking would be more Makefile than it's worth.
%.o: %.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f gbemu $(OBJ)

.PHONY: clean
