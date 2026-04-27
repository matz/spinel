# Spinel AOT Compiler - Makefile
#
# Usage:
#   make              Build everything (parser + bootstrap compiler)
#   make parse        Build C parser only
#   make bootstrap    Bootstrap the compiler backend
#   make test         Run feature tests (requires bootstrap first)
#   make bench        Run benchmarks (requires bootstrap first)
#   make clean        Remove built binaries

CC       ?= cc
CFLAGS   = -O2 -Wno-all
# Bootstrap-only flags: spinel_codegen runs on the developer's machine
# only, so we can use -O3 -flto for ~5-10% extra wall-clock without
# constraining users (whose generated C is built with plain CFLAGS).
# Override with LTO=0 on toolchains without LTO support.
LTO     ?= 1
ifeq ($(LTO),1)
  BOOTSTRAP_CFLAGS = -O3 -flto=auto -Wno-all
else
  BOOTSTRAP_CFLAGS = $(CFLAGS)
endif
# Per-function sections allow the linker to strip unused bigint/regexp
# functions from the final binary (supported since GCC 2.7 / binutils 2.17).
SEC_FLAGS = -ffunction-sections -fdata-sections
# Apple ld64 spells dead-code stripping --dead_strip; GNU ld uses --gc-sections.
ifeq ($(shell uname -s),Darwin)
  GC_FLAGS = -Wl,-dead_strip
else
  GC_FLAGS = -Wl,--gc-sections
endif

# MinGW gcc appends .exe to its output filename; reflect that in target
# names so Make's dependency tracking and install/clean match reality.
# Windows (MinGW) default stack is also only 1MB — far too small for the
# deeply recursive bootstrap compile (~75k frames of AST traversal).
ifeq ($(OS),Windows_NT)
  EXE = .exe
  LDFLAGS += -Wl,--stack,67108864
endif

# `timeout` is GNU coreutils — present by default on Linux but missing
# on macOS unless `brew install coreutils` is run (where it's named
# `gtimeout`). Without it test/bench would chain-fail at the very first
# `timeout` call with exit 127, reporting 0 pass / 0 fail. Detect both
# names; if neither is found, run without time limits (slow benches
# won't be auto-killed, but otherwise everything works).
TIMEOUT_BIN := $(shell command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null)
TIMEOUT10 := $(if $(TIMEOUT_BIN),$(TIMEOUT_BIN) 10,)
TIMEOUT60 := $(if $(TIMEOUT_BIN),$(TIMEOUT_BIN) 60,)

# Prism library: prefer vendor/prism (fetched via `make deps`), then
# fall back to the Prism gem if one is installed. Override by setting
# PRISM_DIR=/path/to/prism on the command line.
PRISM_VERSION ?= 1.9.0
ifneq ($(wildcard vendor/prism/include/prism.h),)
  PRISM_DIR ?= vendor/prism
else
  PRISM_DIR ?= $(shell ruby -rprism -e 'puts $$LOADED_FEATURES.grep(/prism/).first.sub(%r{/lib/.*}, "")' 2>/dev/null)
endif

PRISM_INC    = $(PRISM_DIR)/include
PRISM_SRC    = $(wildcard $(PRISM_DIR)/src/*.c) $(wildcard $(PRISM_DIR)/src/util/*.c)
PRISM_OBJ    = $(patsubst $(PRISM_DIR)/src/%.c,build/prism/%.o,$(PRISM_SRC))
PRISM_LIB    = build/libprism.a

.PHONY: all parse bootstrap test bench clean install uninstall deps

all: parse regexp bootstrap

# ---- Dependencies ----
# Clone Prism into vendor/prism at the pinned version. Run this once
# after cloning Spinel if you don't have the Prism gem installed.
deps: vendor/prism/include/prism/diagnostic.h

# Download the pre-built Prism gem from rubygems.org and extract its C
# sources. We use the .gem tarball instead of a git clone because it
# ships with the generated headers (diagnostic.h, etc.) already in
# place — no rake/bundler needed.
vendor/prism/include/prism/diagnostic.h:
	@mkdir -p vendor/prism
	@echo "Fetching prism v$(PRISM_VERSION) from rubygems.org..."
	curl -sL -o /tmp/prism-$(PRISM_VERSION).gem https://rubygems.org/gems/prism-$(PRISM_VERSION).gem
	@tmpdir=$$(mktemp -d); \
	 tar -xf /tmp/prism-$(PRISM_VERSION).gem -C $$tmpdir data.tar.gz; \
	 tar -xzf $$tmpdir/data.tar.gz -C vendor/prism; \
	 rm -rf $$tmpdir /tmp/prism-$(PRISM_VERSION).gem
	@test -f $@ && echo "prism v$(PRISM_VERSION) ready at vendor/prism"

# If PRISM_DIR ended up empty (no vendor/prism, no gem), halt with a
# clear message before trying to build anything that needs it.
ifeq ($(PRISM_DIR),)
parse bootstrap regexp all: prism-missing
prism-missing:
	@echo "Error: Prism not found."; \
	 echo "  Run 'make deps' to fetch libprism into vendor/prism,"; \
	 echo "  or install the prism gem (gem install prism),"; \
	 echo "  or set PRISM_DIR=/path/to/prism manually."; \
	 exit 1
endif

# ---- Prism static library ----

build/libprism.a: $(PRISM_OBJ)
	@mkdir -p build
	ar rcs $@ $^

build/prism/%.o: $(PRISM_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -c -O2 -I$(PRISM_INC) -I$(PRISM_DIR)/src $< -o $@

# ---- C Parser ----

parse: spinel_parse$(EXE)

spinel_parse$(EXE): spinel_parse.c $(PRISM_LIB)
	$(CC) $(CFLAGS) -I$(PRISM_INC) $< $(PRISM_LIB) -lm -o $@

# ---- Runtime library (regexp + bigint) ----

RE_SRC = lib/regexp/re_compile.c lib/regexp/re_exec.c lib/regexp/re_utf8.c
RE_OBJ = $(patsubst lib/regexp/%.c,build/regexp/%.o,$(RE_SRC))

build/regexp/%.o: lib/regexp/%.c lib/regexp/re_internal.h
	@mkdir -p build/regexp
	$(CC) -c -O2 $(SEC_FLAGS) -Ilib/regexp $< -o $@

build/sp_bigint.o: lib/sp_bigint.c lib/sp_bigint.h lib/mruby_shim.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_bigint.c -o build/sp_bigint.o

SP_RT_LIB = lib/libspinel_rt.a

$(SP_RT_LIB): $(RE_OBJ) build/sp_bigint.o
	ar rcs $@ $^

regexp: $(SP_RT_LIB)

# ---- Bootstrap ----

bootstrap: spinel_codegen$(EXE)

spinel_codegen$(EXE): spinel_codegen.rb spinel_parse$(EXE)
	@echo "=== Bootstrap Step 1: parse ==="
	./spinel_parse$(EXE) spinel_codegen.rb build/codegen.ast
	@echo "=== Bootstrap Step 2: gen1 (CRuby) ==="
	ruby spinel_codegen.rb build/codegen.ast build/gen1.c
	$(CC) $(BOOTSTRAP_CFLAGS) -Ilib build/gen1.c $(LDFLAGS) -lm -o build/bin1$(EXE)
	@echo "=== Bootstrap Step 3: gen2 (bin1) ==="
	./build/bin1$(EXE) build/codegen.ast build/gen2.c
	$(CC) $(BOOTSTRAP_CFLAGS) -Ilib build/gen2.c $(LDFLAGS) -lm -o build/bin2$(EXE)
	@echo "=== Bootstrap Step 4: gen3 (bin2) - verify ==="
	./build/bin2$(EXE) build/codegen.ast build/gen3.c
	@diff build/gen2.c build/gen3.c > /dev/null && echo "gen2.c == gen3.c (bootstrap OK)" || (echo "BOOTSTRAP FAILED: gen2.c != gen3.c" && exit 1)
	cp build/bin2$(EXE) spinel_codegen$(EXE)

# ---- Test ----

test: spinel_parse$(EXE) $(SP_RT_LIB)
	@if [ ! -f spinel_codegen$(EXE) ]; then echo "Run 'make bootstrap' first"; exit 1; fi
	@if [ -z "$(TIMEOUT_BIN)" ]; then echo "Note: no 'timeout' command found; running without time limits."; fi
	@pass=0; fail=0; err=0; \
	for f in test/*.rb; do \
	  bn=$$(basename "$$f" .rb); \
	  ./spinel_parse$(EXE) "$$f" /tmp/_sp_t.ast 2>/dev/null && \
	  ./spinel_codegen$(EXE) /tmp/_sp_t.ast /tmp/_sp_t.c 2>/dev/null && \
	  $(CC) $(CFLAGS) -Werror $(SEC_FLAGS) -Ilib /tmp/_sp_t.c $(SP_RT_LIB) $(LDFLAGS) -lm $(GC_FLAGS) -o /tmp/_sp_t_bin$(EXE) 2>/dev/null; \
	  if [ $$? -eq 0 ]; then \
	    expected=$$($(TIMEOUT10) ruby "$$f" 2>/dev/null); \
	    actual=$$($(TIMEOUT10) /tmp/_sp_t_bin$(EXE) 2>/dev/null); \
	    if [ "$$expected" = "$$actual" ]; then \
	      pass=$$((pass+1)); \
	    else \
	      echo "FAIL: $$bn"; fail=$$((fail+1)); \
	    fi; \
	  else \
	    echo "ERR:  $$bn"; err=$$((err+1)); \
	  fi; \
	done; \
	rm -f /tmp/_sp_t.ast /tmp/_sp_t.c /tmp/_sp_t_bin$(EXE); \
	echo "Tests: $$pass pass, $$fail fail, $$err error"

bench: spinel_parse$(EXE) $(SP_RT_LIB)
	@if [ ! -f spinel_codegen$(EXE) ]; then echo "Run 'make bootstrap' first"; exit 1; fi
	@if [ -z "$(TIMEOUT_BIN)" ]; then echo "Note: no 'timeout' command found; running without time limits."; fi
	@pass=0; fail=0; skip=0; \
	for f in benchmark/*.rb; do \
	  bn=$$(basename "$$f" .rb); \
	  $(TIMEOUT10) ./spinel_parse$(EXE) "$$f" /tmp/_sp_b.ast 2>/dev/null && \
	  $(TIMEOUT10) ./spinel_codegen$(EXE) /tmp/_sp_b.ast /tmp/_sp_b.c 2>/dev/null && \
	  $(CC) $(CFLAGS) -Werror $(SEC_FLAGS) -Ilib /tmp/_sp_b.c $(SP_RT_LIB) $(LDFLAGS) -lm $(GC_FLAGS) -o /tmp/_sp_b_bin$(EXE) 2>/dev/null; \
	  if [ $$? -eq 0 ]; then \
	    expected=$$($(TIMEOUT60) ruby "$$f" 2>/dev/null); \
	    ruby_rc=$$?; \
	    if [ $$ruby_rc -eq 124 ]; then \
	      echo "SKIP: $$bn (ruby timeout)"; skip=$$((skip+1)); \
	    else \
	      actual=$$($(TIMEOUT60) /tmp/_sp_b_bin$(EXE) 2>/dev/null); \
	      if [ "$$expected" = "$$actual" ]; then \
	        pass=$$((pass+1)); \
	      else \
	        echo "FAIL: $$bn"; fail=$$((fail+1)); \
	      fi; \
	    fi; \
	  else \
	    echo "ERR:  $$bn"; \
	  fi; \
	done; \
	rm -f /tmp/_sp_b.ast /tmp/_sp_b.c /tmp/_sp_b_bin$(EXE); \
	echo "Benchmarks: $$pass pass, $$fail fail, $$skip skip"

# ---- Install ----

PREFIX   ?= /usr/local
SPNLDIR   = $(PREFIX)/lib/spinel

install: all
	install -d $(SPNLDIR)/lib
	install -m 755 spinel                $(SPNLDIR)/
	install -m 755 spinel_parse$(EXE)    $(SPNLDIR)/
	install -m 755 spinel_codegen$(EXE)  $(SPNLDIR)/
	install -m 644 spinel_parse.rb       $(SPNLDIR)/
	install -m 644 spinel_codegen.rb     $(SPNLDIR)/
	install -m 644 lib/libspinel_rt.a    $(SPNLDIR)/lib/
	install -m 644 lib/sp_runtime.h      $(SPNLDIR)/lib/
	install -m 644 lib/*.rb              $(SPNLDIR)/lib/
	install -d $(PREFIX)/bin
	ln -sf $(SPNLDIR)/spinel $(PREFIX)/bin/spinel

uninstall:
	rm -f $(PREFIX)/bin/spinel
	rm -rf $(SPNLDIR)

# ---- Clean ----

clean:
	rm -rf build/
	rm -f spinel_parse$(EXE) spinel_codegen$(EXE)
