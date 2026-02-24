CC      = cc
TARGET  = gitools

# Try pkg-config first, fall back to hardcoded Homebrew paths
PKG_CONFIG := $(shell command -v pkg-config 2>/dev/null)
ifdef PKG_CONFIG
  LIBGIT2_CFLAGS := $(shell pkg-config --cflags libgit2)
  LIBGIT2_LIBS   := $(shell pkg-config --libs libgit2)
else
  # Homebrew on Apple Silicon
  BREW_PREFIX    := /opt/homebrew
  LIBGIT2_CFLAGS := -I$(BREW_PREFIX)/include
  LIBGIT2_LIBS   := -L$(BREW_PREFIX)/lib -lgit2
endif

CFLAGS  = -std=c11 -Wall -Wextra -O2 $(LIBGIT2_CFLAGS)
LDFLAGS = $(LIBGIT2_LIBS)

all: $(TARGET)

$(TARGET): gitools.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)

.PHONY: all clean install
