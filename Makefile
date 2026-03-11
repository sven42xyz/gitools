CC      = cc
TARGET  = gitls
PREFIX  = /usr/local
VERSION := $(shell (git describe --tags --always --dirty 2>/dev/null || echo "0.3.1") | sed 's/^v//')
SRCS    = main.c repo.c display.c scan.c config.c
OBJS    = $(SRCS:.c=.o)
DEPS    = $(OBJS:.o=.d)

# Detect OS
UNAME := $(shell uname)

# Try pkg-config first (preferred on all platforms)
PKG_CONFIG := $(shell which pkg-config 2>/dev/null)
ifdef PKG_CONFIG
  LIBGIT2_CFLAGS := $(shell pkg-config --cflags libgit2)
  LIBGIT2_LIBS   := $(shell pkg-config --libs libgit2)
else ifeq ($(UNAME), Darwin)
  # macOS without pkg-config: locate libgit2 via brew (works on Intel and Apple Silicon)
  BREW_PREFIX    := $(shell brew --prefix libgit2 2>/dev/null || echo /opt/homebrew/opt/libgit2)
  LIBGIT2_CFLAGS := -I$(BREW_PREFIX)/include
  LIBGIT2_LIBS   := -L$(BREW_PREFIX)/lib -lgit2
else
  # Linux without pkg-config: assume system-wide install
  LIBGIT2_CFLAGS := -I/usr/include
  LIBGIT2_LIBS   := -lgit2
endif

DARWIN_EXTRA := $(if $(filter Darwin,$(UNAME)),-D_DARWIN_C_SOURCE,)
CFLAGS  = -std=c11 -Wall -Wextra -O2 -MMD -MP -D_POSIX_C_SOURCE=200809L $(DARWIN_EXTRA) $(LIBGIT2_CFLAGS) -DVERSION_STRING=\"$(VERSION)\"
PTHREAD := $(if $(filter Linux,$(UNAME)),-lpthread,)
LDFLAGS = $(LIBGIT2_LIBS) $(PTHREAD)

-include $(DEPS)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c gitools.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Rebuild main.o whenever the version string changes (new git tag, dirty state, etc.)
# .version is updated only when the content actually changes, avoiding spurious rebuilds.
.PHONY: _force
.version: _force
	@printf '%s' "$(VERSION)" > .version.tmp; \
	cmp -s .version.tmp .version 2>/dev/null || mv .version.tmp .version; \
	rm -f .version.tmp

main.o: .version

TEST_OBJS = repo.o display.o scan.o

test: $(TARGET) tests/unit
	@printf "=== Unit tests ===\n"
	@./tests/unit
	@printf "\n=== Integration tests ===\n"
	@sh tests/integration.sh

tests/unit: tests/unit.c $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ tests/unit.c $(TEST_OBJS) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(OBJS) tests/unit .version
	find . -name '*.d' -not -path './.git/*' -delete

INSTALL_NAME ?= $(TARGET)

install: $(TARGET)
	install -d $(PREFIX)/bin
	install -m 755 $(TARGET) $(PREFIX)/bin/$(INSTALL_NAME)
	install -d $(PREFIX)/share/doc/$(TARGET)
	install -m 644 gitlsrc.example $(PREFIX)/share/doc/$(TARGET)/gitlsrc.example

uninstall:
	rm -f $(PREFIX)/bin/$(INSTALL_NAME)
	rm -f $(PREFIX)/share/doc/$(TARGET)/gitlsrc.example

help:
	@printf "Usage: make [TARGET] [VARIABLES]\n\n"
	@printf "Targets:\n"
	@printf "  all          Build $(TARGET) (default)\n"
	@printf "  test         Run unit and integration tests\n"
	@printf "  install      Install to $(PREFIX)/bin\n"
	@printf "  uninstall    Remove from $(PREFIX)/bin\n"
	@printf "  clean        Remove build artifacts\n"
	@printf "  help         Show this help\n\n"
	@printf "Variables:\n"
	@printf "  CC           C compiler (default: cc)\n"
	@printf "  PREFIX       Install prefix (default: $(PREFIX))\n"
	@printf "  INSTALL_NAME Binary name (default: $(TARGET))\n"

.PHONY: all clean install uninstall test help
