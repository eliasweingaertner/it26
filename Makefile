# ---------------------------------------------------------------------------
# it26 -- GNU Makefile (POSIX build path: Linux, macOS, MSYS2 / Git Bash)
#
# CMakeLists.txt stays the canonical cross-platform build and the only
# supported path for MSVC/Windows.  This Makefile is the quick no-deps
# alternative and the home of the VGA ROM font fetch (see "ROM font" below).
#
#   make            build itplay, ited and test_pattern
#   make itplay     player / offline WAV renderer only
#   make ited       editor only
#   make test       determinism regression over testdata/ (must stay IDENTICAL)
#   make font       fetch + verify tools/IBM_VGA_8x8.bin
#   make vgadata    regenerate src/it_vgadata.c (needs the font + the IT ASM)
#   make cornerart  regenerate src/it_cornerart.c (from art/corner.bmp)
#   make clean      remove objects and binaries
#   make distclean  also remove the fetched font
#   make help       this list
#
# Knobs:  ITED_SDL=0  build ited without the SDL2 pixel backend
#         IT_ASM_SRC=<dir>  original Impulse Tracker ASM tree (for `vgadata`)
#         CC / CFLAGS / LDFLAGS  as usual
# ---------------------------------------------------------------------------

CC       ?= cc
PYTHON   ?= python3
CFLAGS   ?= -O2
CFLAGS   += -std=c11 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -Isrc
LDLIBS   += -lm

OBJDIR   := build-make
UNAME_S  := $(shell uname -s 2>/dev/null || echo Unknown)

# ---------------------------------------------------------------------------
# ROM font (not vendored -- fetched on demand)
#
# The editor's authentic look needs the IBM VGA ROM 8x8 CP437 font: the font
# `int 10h AX=1112h` loads, i.e. exactly what Impulse Tracker ran on top of.
# Those bytes are a dump of IBM's VGA BIOS character ROM, so this repo does
# not redistribute them -- `make font` fetches them from spacerace/romfont,
# pinned to a commit and checked against a known-good SHA-256.
#
# It is NOT needed for an ordinary build: src/it_vgadata.c is generated and
# committed.  Only `make vgadata` (regenerating it) needs the font.
# ---------------------------------------------------------------------------
ROMFONT_REPO   := https://github.com/spacerace/romfont
ROMFONT_COMMIT := 73f2ba6849e7f1d8a58d5861e462bcf6bf169780
ROMFONT_PATH   := font-bin/IBM_VGA_8x8.bin
ROMFONT_URL    := https://raw.githubusercontent.com/spacerace/romfont/$(ROMFONT_COMMIT)/$(ROMFONT_PATH)
ROMFONT_SHA256 := 75c79a7e7fa423dda67ec6d6d76cec86b63f85677726368750c75b0920ddf319
ROMFONT_SIZE   := 2048
FONT           := tools/IBM_VGA_8x8.bin

IT_ASM_SRC ?= ../impulsetracker

# ---------------------------------------------------------------------------
# Platform / optional SDL2
# ---------------------------------------------------------------------------
IS_WINDOWS := $(if $(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)),1,)

ifeq ($(IS_WINDOWS),1)
  # Win32 pixel backend (GDI window), no pthread/dl.
  PLATFORM_OBJS := $(OBJDIR)/src/it_screen_win32.o
  LDLIBS        += -luser32 -lgdi32 -lole32 -lshell32 -luuid   # + COM dialogs (016)
  EXE           := .exe
else
  PLATFORM_OBJS :=
  EXE           :=
  LDLIBS        += -lpthread
  ifeq ($(UNAME_S),Linux)
    LDLIBS += -ldl
  endif
endif

# SDL2 pixel backend: POSIX only, on by default, silently skipped when SDL2
# is absent (the editor then uses the no-deps terminal backend).
ITED_SDL ?= 1
SDL_CFLAGS :=
SDL_LIBS   :=
ifneq ($(IS_WINDOWS),1)
ifneq ($(ITED_SDL),0)
  SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs 2>/dev/null)
  ifneq ($(SDL_LIBS),)
    SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags 2>/dev/null) -DHAVE_SDL
    PLATFORM_OBJS += $(OBJDIR)/src/it_screen_sdl.o
    ifeq ($(UNAME_S),Darwin)
      # press-and-hold off (#15) + NSOpenPanel/NSSavePanel (016), both via
      # the Objective-C runtime from C
      SDL_LIBS += -framework CoreFoundation -lobjc
    endif
  endif
endif
endif

# ---------------------------------------------------------------------------
# Sources (mirror CMakeLists.txt)
# ---------------------------------------------------------------------------
ENGINE_SRCS := src/it_music.c src/it_effects.c src/it_tables.c src/it_driver.c \
               src/it_load.c src/it_pattern.c src/it_save.c
EDITOR_SRCS := src/it_import.c src/it_ris.c src/it_screen.c \
               src/it_screen_remote.c src/it_vgadata.c \
               src/it_cornerart.c src/it_editor.c \
               src/it_dialog_win32.c src/it_dialog_mac.c src/it_dialog_posix.c

ENGINE_OBJS := $(ENGINE_SRCS:%.c=$(OBJDIR)/%.o)
EDITOR_OBJS := $(EDITOR_SRCS:%.c=$(OBJDIR)/%.o) $(PLATFORM_OBJS)

BINS := itplay$(EXE) ited$(EXE) test_pattern$(EXE)

# ---------------------------------------------------------------------------
# Build rules
# ---------------------------------------------------------------------------
.PHONY: all
all: $(BINS)

# Editor translation units carry the SDL flags; engine objects never do, so
# they stay shareable between itplay, ited and test_pattern.
$(EDITOR_OBJS): CFLAGS += $(SDL_CFLAGS)

itplay$(EXE): $(ENGINE_OBJS) $(OBJDIR)/src/main.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

ited$(EXE): $(ENGINE_OBJS) $(EDITOR_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(SDL_LIBS) $(LDLIBS)

test_pattern$(EXE): $(ENGINE_OBJS) $(OBJDIR)/tests/test_pattern.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(wildcard $(OBJDIR)/src/*.d $(OBJDIR)/tests/*.d)

# ---------------------------------------------------------------------------
# Determinism regression (constitution III): all four modules must print
# IDENTICAL.  Run after any engine or pattern-format change.
# ---------------------------------------------------------------------------
TESTMODS := beyond_network itdemo quests_end synthscape_filters

.PHONY: test
test: test_pattern$(EXE)
	@fail=0; for m in $(TESTMODS); do \
	    echo "--- $$m"; \
	    ./test_pattern$(EXE) testdata/$$m.it || fail=1; \
	done; \
	exit $$fail

# ---------------------------------------------------------------------------
# ROM font fetch: curl, else wget, else a shallow git remote fetch.
# Always verified against $(ROMFONT_SHA256) before it is put in place.
# ---------------------------------------------------------------------------
.PHONY: font
font: $(FONT)

$(FONT):
	@mkdir -p $(@D)
	@echo "  FETCH    $@"
	@echo "           $(ROMFONT_REPO) @ $(ROMFONT_COMMIT)"
	@echo "           $(ROMFONT_PATH)"
	@rm -rf $@.tmp $@.git
	@if command -v curl >/dev/null 2>&1; then \
	    curl -sSfL -o $@.tmp "$(ROMFONT_URL)"; \
	 elif command -v wget >/dev/null 2>&1; then \
	    wget -qO $@.tmp "$(ROMFONT_URL)"; \
	 elif command -v git >/dev/null 2>&1; then \
	    echo "           (no curl/wget -- fetching via git remote)"; \
	    git init -q $@.git && \
	    git -C $@.git remote add origin "$(ROMFONT_REPO).git" && \
	    git -C $@.git fetch -q --depth 1 origin $(ROMFONT_COMMIT) && \
	    git -C $@.git checkout -q FETCH_HEAD -- $(ROMFONT_PATH) && \
	    cp $@.git/$(ROMFONT_PATH) $@.tmp && rm -rf $@.git; \
	 else \
	    echo "error: need curl, wget or git to fetch the VGA ROM font" >&2; \
	    echo "       download $(ROMFONT_URL)" >&2; \
	    echo "       to $@ by hand, then re-run." >&2; \
	    exit 1; \
	 fi
	@test -s $@.tmp || { echo "error: fetch produced no data" >&2; rm -f $@.tmp; exit 1; }
	@size=`wc -c < $@.tmp | tr -d ' '`; \
	 if [ "$$size" != "$(ROMFONT_SIZE)" ]; then \
	    echo "error: $@: got $$size bytes, want $(ROMFONT_SIZE)" >&2; \
	    rm -f $@.tmp; exit 1; \
	 fi
	@if command -v sha256sum >/dev/null 2>&1; then \
	    got=`sha256sum < $@.tmp | cut -d' ' -f1`; \
	 elif command -v shasum >/dev/null 2>&1; then \
	    got=`shasum -a 256 < $@.tmp | cut -d' ' -f1`; \
	 elif command -v openssl >/dev/null 2>&1; then \
	    got=`openssl dgst -sha256 < $@.tmp | sed 's/.*= *//'`; \
	 else \
	    got=''; echo "warning: no sha256 tool; skipping checksum" >&2; \
	 fi; \
	 if [ -n "$$got" ] && [ "$$got" != "$(ROMFONT_SHA256)" ]; then \
	    echo "error: $@: checksum mismatch" >&2; \
	    echo "  got  $$got" >&2; \
	    echo "  want $(ROMFONT_SHA256)" >&2; \
	    rm -f $@.tmp; exit 1; \
	 fi
	@mv $@.tmp $@
	@echo "  OK       $@ ($(ROMFONT_SIZE) bytes, sha256 verified)"

# ---------------------------------------------------------------------------
# Regenerate src/it_vgadata.c from the original IT_S.ASM + the ROM font.
# Explicit target on purpose: it_vgadata.c is committed, so an ordinary
# build never reaches for the network.
# ---------------------------------------------------------------------------
.PHONY: vgadata
vgadata: $(FONT)
	@test -f "$(IT_ASM_SRC)/IT_S.ASM" || { \
	    echo "error: IT_S.ASM not under IT_ASM_SRC=$(IT_ASM_SRC)" >&2; \
	    echo "       point it at an Impulse Tracker source tree, e.g." >&2; \
	    echo "       make vgadata IT_ASM_SRC=../impulsetracker" >&2; \
	    exit 1; \
	 }
	$(PYTHON) tools/gen_vgadata.py "$(IT_ASM_SRC)" $(FONT) src/it_vgadata.c

# ---------------------------------------------------------------------------
# Regenerate src/it_cornerart.c/.h from the corner-art badge bitmap.
# it_cornerart.c is committed; only needed when art/corner.bmp changes.
# ---------------------------------------------------------------------------
.PHONY: cornerart
cornerart:
	$(PYTHON) tools/gen_cornerart.py art/corner.bmp src/it_cornerart

# ---------------------------------------------------------------------------
.PHONY: clean distclean help
clean:
	rm -rf $(OBJDIR) $(BINS)

distclean: clean
	rm -f $(FONT)

help:
	@echo 'it26 targets:'
	@echo '  all (default)  build itplay, ited, test_pattern'
	@echo '  itplay         player / offline WAV renderer'
	@echo '  ited           editor'
	@echo '  test           determinism regression (must stay IDENTICAL)'
	@echo '  font           fetch + verify $(FONT)'
	@echo '  vgadata        regenerate src/it_vgadata.c (font + IT_ASM_SRC)'
	@echo '  cornerart      regenerate src/it_cornerart.c (art/corner.bmp)'
	@echo '  clean          remove objects and binaries'
	@echo '  distclean      also remove the fetched font'
	@echo ''
	@echo 'knobs:'
	@echo '  ITED_SDL=0            build ited without the SDL2 pixel backend'
	@echo '  IT_ASM_SRC=<dir>      IT ASM tree for `vgadata` (now: $(IT_ASM_SRC))'
	@echo ''
	@echo 'this build:'
	@echo '  uname        $(UNAME_S)'
ifeq ($(IS_WINDOWS),1)
	@echo '  backends     win32 pixel + terminal'
else
ifneq ($(SDL_LIBS),)
	@echo '  backends     sdl2 pixel + terminal'
else
	@echo '  backends     terminal only (SDL2 not found or ITED_SDL=0)'
endif
endif
