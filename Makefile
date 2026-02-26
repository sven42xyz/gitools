CC      = cc
TARGET  = gitls
PREFIX  = /usr/local
VERSION = 0.2.0
SRCS    = main.c repo.c display.c scan.c
OBJS    = $(SRCS:.c=.o)

# Detect OS
UNAME := $(shell uname)

# Try pkg-config first (preferred on all platforms)
PKG_CONFIG := $(shell command -v pkg-config 2>/dev/null)
ifdef PKG_CONFIG
  LIBGIT2_CFLAGS := $(shell pkg-config --cflags libgit2)
  LIBGIT2_LIBS   := $(shell pkg-config --libs libgit2)
else ifeq ($(UNAME), Darwin)
  # macOS without pkg-config: fall back to Homebrew
  BREW_PREFIX    := /opt/homebrew
  LIBGIT2_CFLAGS := -I$(BREW_PREFIX)/include
  LIBGIT2_LIBS   := -L$(BREW_PREFIX)/lib -lgit2
else
  # Linux without pkg-config: assume system-wide install
  LIBGIT2_CFLAGS := -I/usr/include
  LIBGIT2_LIBS   := -lgit2
endif

CFLAGS  = -std=c11 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L $(LIBGIT2_CFLAGS) -DVERSION_STRING=\"$(VERSION)\"
PTHREAD := $(if $(filter Linux,$(UNAME)),-lpthread,)
LDFLAGS = $(LIBGIT2_LIBS) $(PTHREAD)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c gitools.h
	$(CC) $(CFLAGS) -c -o $@ $<

TEST_OBJS = repo.o display.o scan.o

test: $(TARGET) tests/unit
	@printf "=== Unit tests ===\n"
	@./tests/unit
	@printf "\n=== Integration tests ===\n"
	@sh tests/integration.sh

tests/unit: tests/unit.c $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ tests/unit.c $(TEST_OBJS) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(OBJS) tests/unit

install: $(TARGET)
	install -d $(PREFIX)/bin
	install -m 755 $(TARGET) $(PREFIX)/bin/$(TARGET)

.PHONY: all clean install test
