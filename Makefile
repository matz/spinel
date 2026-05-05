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
# `-Wno-alloc-size-larger-than` silences a gcc 13+ paranoia warning that
# fires on the inlined `h->cap *= 2` in sp_*Hash_grow when the inliner
# can't bound h->cap. The pattern is bounded in practice (cap doubles
# from a small power of two until memory runs out, never approaches
# 2^60), but gcc tracks signed-overflow UB conservatively. Clang
# doesn't have that warning; without `-Wno-unknown-warning-option`
# first, clang would turn the unknown -Wno- into a -Werror failure on
# every cc invocation in `make test` / `make bench`.
# Optimization level for the C compiles driven by the test/bench
# harness (each .rb gets parsed → codegen → cc'd → run). CI overrides
# this to -O0 to cut Windows cc time substantially; locally -O2 keeps
# the test binaries reasonable-speed for quicker run-after-compile.
OPT     ?= -O2
CFLAGS   = $(OPT) -Wno-all -Wno-unknown-warning-option -Wno-alloc-size-larger-than
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

# Reference Ruby for `make test` / `make bench` output comparison.
# Defaults to `ruby` (the system CRuby), matching the historical
# behaviour. Override on the command line when a newer or differently-
# built interpreter is needed, e.g.
#
#   REF_RUBY=miniruby make test
#
# to use a freshly-built bootstrap interpreter (Ruby's `miniruby`)
# that supports newer features like the `it` block param. The
# harness falls back to `ruby` per-file if `REF_RUBY` exits non-zero
# — covers tests that `require` extension libraries (stringio, etc.)
# which the bootstrap miniruby can't load.
REF_RUBY ?= ruby

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

.PHONY: all parse bootstrap codegen test test-run clean-test-results regen-expected bench clean install uninstall deps

all: parse regexp spinel_codegen$(EXE)

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
parse bootstrap codegen regexp all: prism-missing
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

# ---- Build the codegen binary (fast path) ----
# `make spinel_codegen` (or the alias `make codegen`) compiles
# spinel_codegen.rb once via CRuby and links the result. This is
# enough to use the binary; for the full self-hosting fixed-point
# check (gen2.c == gen3.c), use `make bootstrap`.

codegen: spinel_codegen$(EXE)

spinel_codegen$(EXE): spinel_codegen.rb lib/node_store.rb spinel_parse$(EXE)
	./spinel_parse$(EXE) spinel_codegen.rb build/codegen.ast
	ruby spinel_codegen.rb build/codegen.ast build/gen1.c
	$(CC) $(BOOTSTRAP_CFLAGS) -Ilib build/gen1.c $(LDFLAGS) -lm -o spinel_codegen$(EXE)

# ---- Self-hosting verification (slow path) ----
# Re-runs the binary on its own AST to produce gen2.c, compiles bin2,
# re-runs bin2 to produce gen3.c, asserts gen2.c == gen3.c (the
# self-hosting fixed point). On success, replaces spinel_codegen with
# the verified bin2.

bootstrap: spinel_codegen$(EXE)
	@echo "=== Bootstrap: gen2 (via spinel_codegen) ==="
	./spinel_codegen$(EXE) build/codegen.ast build/gen2.c
	$(CC) $(BOOTSTRAP_CFLAGS) -Ilib build/gen2.c $(LDFLAGS) -lm -o build/bin2$(EXE)
	@echo "=== Bootstrap: gen3 (via bin2) - verify ==="
	./build/bin2$(EXE) build/codegen.ast build/gen3.c
	@diff build/gen2.c build/gen3.c > /dev/null && echo "gen2.c == gen3.c (bootstrap OK)" || (echo "BOOTSTRAP FAILED: gen2.c != gen3.c" && exit 1)
	cp build/bin2$(EXE) spinel_codegen$(EXE)

# ---- Test ----

TESTS := $(wildcard test/*.rb)
TEST_TARGETS := $(patsubst test/%.rb,build/test-results/%.ok,$(TESTS))

test: clean-test-results
	@$(MAKE) --no-print-directory test-run

test-run: $(TEST_TARGETS)
	@if [ -z "$(TIMEOUT_BIN)" ]; then echo "Note: no 'timeout' command found; running without time limits."; fi
	@if [ -t 1 ]; then printf '\n'; fi
	@pass=$$(grep -l '^PASS' build/test-results/*.ok 2>/dev/null | wc -l); \
	fail=$$(grep -l '^FAIL' build/test-results/*.ok 2>/dev/null | wc -l); \
	err=$$(grep -l '^ERR' build/test-results/*.ok 2>/dev/null | wc -l); \
	for f in build/test-results/*.ok; do \
	  bn=$$(basename "$$f" .ok); \
	  status=$$(cat "$$f"); \
	  if [ "$$status" = FAIL ]; then \
	    echo "FAIL: $$bn"; \
	    head -40 "$$f.diff"; \
	  elif [ "$$status" = ERR ]; then \
	    echo "ERR:  $$bn"; \
	  fi; \
	done; \
	echo "Tests: $$pass pass, $$fail fail, $$err error"; \
	if [ $$fail -ne 0 ] || [ $$err -ne 0 ]; then exit 1; fi

build/test-results/%.ok: test/%.rb spinel_parse$(EXE) $(SP_RT_LIB) spinel_codegen$(EXE)
	@mkdir -p build/test-results
	@tmpdir=$$(mktemp -d /tmp/spinel-test.XXXXXX); \
	ast=$$tmpdir/test.ast; \
	cfile=$$tmpdir/test.c; \
	bin=$$tmpdir/test_bin$(EXE); \
	exp=$$tmpdir/expected; \
	act=$$tmpdir/actual; \
	rm -f "$@.diff"; \
	./spinel_parse$(EXE) "$<" "$$ast" 2>/dev/null && \
	./spinel_codegen$(EXE) "$$ast" "$$cfile" 2>/dev/null && \
	$(CC) $(CFLAGS) -Werror $(SEC_FLAGS) -Ilib -c "$$cfile" -o "$$cfile.o" 2>/dev/null && \
	$(CC) $(CFLAGS) "$$cfile.o" $(SP_RT_LIB) $(LDFLAGS) -lm $(GC_FLAGS) -o "$$bin" 2>/dev/null; \
	if [ $$? -eq 0 ]; then \
	  if [ -f "$<.expected" ]; then \
	    LC_ALL=C sed 's/\r$$//' "$<.expected" >"$$exp.n"; \
	  else \
	    $(TIMEOUT10) $(REF_RUBY) "$<" >"$$exp" 2>/dev/null; \
	    ruby_rc=$$?; \
	    if [ $$ruby_rc -ne 0 ] && [ "$(REF_RUBY)" != "ruby" ]; then \
	      $(TIMEOUT10) ruby "$<" >"$$exp" 2>/dev/null; \
	    fi; \
	    LC_ALL=C sed 's/\r$$//' "$$exp" >"$$exp.n"; \
	  fi; \
	  $(TIMEOUT10) "$$bin" >"$$act" 2>/dev/null; \
	  LC_ALL=C sed 's/\r$$//' "$$act" >"$$act.n"; \
	  if cmp -s "$$exp.n" "$$act.n"; then \
	    echo PASS > "$@"; \
	    if [ -t 1 ]; then printf .; fi; \
	  else \
	    echo FAIL > "$@"; \
	    diff -u "$$exp.n" "$$act.n" > "$@.diff" 2>&1 || true; \
	    if [ -t 1 ]; then printf F; fi; \
	  fi; \
	else \
	  echo ERR > "$@"; \
	  if [ -t 1 ]; then printf E; fi; \
	fi; \
	rm -rf "$$tmpdir"

clean-test-results:
	@rm -rf build/test-results

# ---- Expected-output regeneration ----
# Capture each test's reference Ruby output into test/<name>.rb.expected.
# Once committed, the test target uses the .expected file directly and
# skips the per-test ruby invocation — useful in CI where ruby's startup
# (especially mingw64 ruby on Windows) adds up across hundreds of tests.
# Regenerate after adding or modifying tests; commit the result.

EXPECTED_FILES := $(patsubst test/%.rb,test/%.rb.expected,$(TESTS))

regen-expected: $(EXPECTED_FILES)

test/%.rb.expected: test/%.rb
	@$(TIMEOUT10) $(REF_RUBY) $< >$@.tmp 2>/dev/null; \
	rc=$$?; \
	if [ $$rc -ne 0 ] && [ "$(REF_RUBY)" != "ruby" ]; then \
	  $(TIMEOUT10) ruby $< >$@.tmp 2>/dev/null; \
	fi; \
	LC_ALL=C sed 's/\r$$//' $@.tmp > $@; \
	rm -f $@.tmp

bench: spinel_parse$(EXE) $(SP_RT_LIB) spinel_codegen$(EXE)
	@if [ -z "$(TIMEOUT_BIN)" ]; then echo "Note: no 'timeout' command found; running without time limits."; fi
	@total=$$(ls benchmark/*.rb | wc -l); \
	if [ -t 1 ]; then tty=1; else tty=0; fi; \
	pass=0; fail=0; err=0; skip=0; i=0; \
	for f in benchmark/*.rb; do \
	  i=$$((i+1)); \
	  bn=$$(basename "$$f" .rb); \
	  if [ "$$tty" = 1 ]; then printf '\r\033[K  [%d/%d] %s' "$$i" "$$total" "$$bn"; fi; \
	  $(TIMEOUT10) ./spinel_parse$(EXE) "$$f" /tmp/_sp_b.ast 2>/dev/null && \
	  $(TIMEOUT10) ./spinel_codegen$(EXE) /tmp/_sp_b.ast /tmp/_sp_b.c 2>/dev/null && \
	  $(CC) $(CFLAGS) -Werror $(SEC_FLAGS) -Ilib -c /tmp/_sp_b.c -o /tmp/_sp_b.c.o 2>/dev/null && \
	  $(CC) $(CFLAGS) /tmp/_sp_b.c.o $(SP_RT_LIB) $(LDFLAGS) -lm $(GC_FLAGS) -o /tmp/_sp_b_bin$(EXE) 2>/dev/null; \
	  if [ $$? -eq 0 ]; then \
	    $(TIMEOUT60) $(REF_RUBY) "$$f" >/tmp/_sp_b_exp 2>/dev/null; \
	    ruby_rc=$$?; \
	    if [ $$ruby_rc -ne 0 ] && [ "$(REF_RUBY)" != "ruby" ]; then \
	      $(TIMEOUT60) ruby "$$f" >/tmp/_sp_b_exp 2>/dev/null; \
	      ruby_rc=$$?; \
	    fi; \
	    if [ $$ruby_rc -eq 124 ]; then \
	      if [ "$$tty" = 1 ]; then printf '\r\033[K'; fi; \
	      echo "SKIP: $$bn (ruby timeout)"; skip=$$((skip+1)); \
	    else \
	      $(TIMEOUT60) /tmp/_sp_b_bin$(EXE) >/tmp/_sp_b_act 2>/dev/null; \
	      LC_ALL=C sed 's/\r$$//' /tmp/_sp_b_exp >/tmp/_sp_b_exp.n; \
	      LC_ALL=C sed 's/\r$$//' /tmp/_sp_b_act >/tmp/_sp_b_act.n; \
	      if cmp -s /tmp/_sp_b_exp.n /tmp/_sp_b_act.n; then \
	        pass=$$((pass+1)); \
	      else \
	        if [ "$$tty" = 1 ]; then printf '\r\033[K'; fi; \
	        echo "FAIL: $$bn"; \
	        diff -u /tmp/_sp_b_exp.n /tmp/_sp_b_act.n 2>&1 | head -40; \
	        fail=$$((fail+1)); \
	      fi; \
	    fi; \
	  else \
	    if [ "$$tty" = 1 ]; then printf '\r\033[K'; fi; \
	    echo "ERR:  $$bn"; err=$$((err+1)); \
	  fi; \
	done; \
	if [ "$$tty" = 1 ]; then printf '\r\033[K'; fi; \
	rm -f /tmp/_sp_b.ast /tmp/_sp_b.c /tmp/_sp_b.c.o /tmp/_sp_b_bin$(EXE) /tmp/_sp_b_exp /tmp/_sp_b_act /tmp/_sp_b_exp.n /tmp/_sp_b_act.n; \
	echo "Benchmarks: $$pass pass, $$fail fail, $$err error, $$skip skip"; \
	if [ $$fail -ne 0 ] || [ $$err -ne 0 ]; then exit 1; fi

# ---- Install ----

PREFIX   ?= /usr/local
SPNLDIR   = $(PREFIX)/lib/spinel

install: all
	install -d $(SPNLDIR)/lib
	install -m 755 spinel                $(SPNLDIR)/
	install -m 755 spinel_parse$(EXE)    $(SPNLDIR)/
	install -m 755 spinel_codegen$(EXE)  $(SPNLDIR)/
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
