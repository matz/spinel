# Spinel AOT Compiler - Makefile
#
# Usage:
#   make              Build everything (parser + bootstrap compiler)
#   make parse        Build C parser only
#   make bootstrap    Bootstrap the compiler backend
#   make test         Run all tests
#   make bench        Run all benchmarks
#   make clean        Remove built binaries

CC       ?= cc
CFLAGS   = -O2 -Wno-all

# Prism library (auto-detect from gem, or set PRISM_DIR manually)
PRISM_DIR ?= $(shell ruby -rprism -e 'puts $$LOADED_FEATURES.grep(/prism/).first.sub(%r{/lib/.*}, "")' 2>/dev/null)

ifeq ($(PRISM_DIR),)
  $(warning Cannot find Prism gem. Set PRISM_DIR=/path/to/prism manually.)
endif

PRISM_INC    = $(PRISM_DIR)/include
PRISM_SRC    = $(wildcard $(PRISM_DIR)/src/*.c) $(wildcard $(PRISM_DIR)/src/util/*.c)
PRISM_OBJ    = $(patsubst $(PRISM_DIR)/src/%.c,build/prism/%.o,$(PRISM_SRC))
PRISM_LIB    = build/libprism.a

.PHONY: all parse bootstrap test bench clean install uninstall

all: parse regexp bootstrap

# ---- Prism static library ----

build/libprism.a: $(PRISM_OBJ)
	@mkdir -p build
	ar rcs $@ $^

build/prism/%.o: $(PRISM_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -c -O2 -I$(PRISM_INC) -I$(PRISM_DIR)/src $< -o $@

# ---- C Parser ----

parse: spinel_parse

spinel_parse: spinel_parse.c $(PRISM_LIB)
	$(CC) $(CFLAGS) -I$(PRISM_INC) $< $(PRISM_LIB) -lm -o $@

# ---- Regexp library (for programs using /pattern/) ----

RE_SRC = lib/regexp/re_compile.c lib/regexp/re_exec.c lib/regexp/re_utf8.c
RE_OBJ = $(patsubst lib/regexp/%.c,build/regexp/%.o,$(RE_SRC))
RE_LIB = lib/regexp/libspre.a

regexp: $(RE_LIB)

$(RE_LIB): $(RE_OBJ)
	ar rcs $@ $^

build/regexp/%.o: lib/regexp/%.c lib/regexp/re_internal.h
	@mkdir -p build/regexp
	$(CC) -c -O2 -Ilib/regexp $< -o $@

# ---- Bootstrap ----

bootstrap: spinel_codegen

spinel_codegen: spinel_codegen.rb spinel_parse
	@echo "=== Bootstrap Step 1: parse ==="
	./spinel_parse spinel_codegen.rb build/codegen.ast
	@echo "=== Bootstrap Step 2: gen1 (CRuby) ==="
	ruby spinel_codegen.rb build/codegen.ast build/gen1.c
	$(CC) $(CFLAGS) build/gen1.c -lm -o build/bin1
	@echo "=== Bootstrap Step 3: gen2 (bin1) ==="
	./build/bin1 build/codegen.ast build/gen2.c
	$(CC) $(CFLAGS) build/gen2.c -lm -o build/bin2
	@echo "=== Bootstrap Step 4: gen3 (bin2) - verify ==="
	./build/bin2 build/codegen.ast build/gen3.c
	@diff build/gen2.c build/gen3.c > /dev/null && echo "gen2.c == gen3.c (bootstrap OK)" || (echo "BOOTSTRAP FAILED: gen2.c != gen3.c" && exit 1)
	cp build/bin2 spinel_codegen

# ---- Test ----

build/sp_bigint.o: lib/sp_bigint.c lib/sp_bigint.h lib/mruby_shim.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all -Ilib lib/sp_bigint.c -o build/sp_bigint.o

test: spinel_parse spinel_codegen build/sp_bigint.o
	@pass=0; fail=0; err=0; \
	for f in test/*.rb; do \
	  bn=$$(basename "$$f" .rb); \
	  ./spinel_parse "$$f" /tmp/_sp_t.ast 2>/dev/null && \
	  ./spinel_codegen /tmp/_sp_t.ast /tmp/_sp_t.c 2>/dev/null && \
	  $(CC) $(CFLAGS) /tmp/_sp_t.c build/sp_bigint.o -lm -o /tmp/_sp_t_bin 2>/dev/null; \
	  if [ $$? -eq 0 ]; then \
	    expected=$$(timeout 10 ruby "$$f" 2>/dev/null); \
	    actual=$$(timeout 10 /tmp/_sp_t_bin 2>/dev/null); \
	    if [ "$$expected" = "$$actual" ]; then \
	      pass=$$((pass+1)); \
	    else \
	      echo "FAIL: $$bn"; fail=$$((fail+1)); \
	    fi; \
	  else \
	    echo "ERR:  $$bn"; err=$$((err+1)); \
	  fi; \
	done; \
	rm -f /tmp/_sp_t.ast /tmp/_sp_t.c /tmp/_sp_t_bin; \
	echo "Tests: $$pass pass, $$fail fail, $$err error"

bench: spinel_parse spinel_codegen
	@pass=0; fail=0; \
	for f in benchmark/*.rb; do \
	  bn=$$(basename "$$f" .rb); \
	  ./spinel_parse "$$f" /tmp/_sp_b.ast 2>/dev/null && \
	  ./spinel_codegen /tmp/_sp_b.ast /tmp/_sp_b.c 2>/dev/null && \
	  $(CC) $(CFLAGS) /tmp/_sp_b.c -lm -o /tmp/_sp_b_bin 2>/dev/null; \
	  if [ $$? -eq 0 ]; then \
	    expected=$$(timeout 60 ruby "$$f" 2>/dev/null); \
	    actual=$$(timeout 60 /tmp/_sp_b_bin 2>/dev/null); \
	    if [ "$$expected" = "$$actual" ]; then \
	      pass=$$((pass+1)); \
	    else \
	      echo "FAIL: $$bn"; fail=$$((fail+1)); \
	    fi; \
	  else \
	    echo "ERR:  $$bn"; \
	  fi; \
	done; \
	rm -f /tmp/_sp_b.ast /tmp/_sp_b.c /tmp/_sp_b_bin; \
	echo "Benchmarks: $$pass pass, $$fail fail"

# ---- Install ----

PREFIX   ?= /usr/local
SPNLDIR   = $(PREFIX)/lib/spinel

install: all
	install -d $(SPNLDIR)/lib/regexp
	install -m 755 spinel           $(SPNLDIR)/
	install -m 755 spinel_parse $(SPNLDIR)/
	install -m 755 spinel_codegen   $(SPNLDIR)/
	install -m 644 spinel_parse.rb  $(SPNLDIR)/
	install -m 644 spinel_codegen.rb $(SPNLDIR)/
	install -m 644 lib/regexp/libspre.a $(SPNLDIR)/lib/regexp/
	install -d $(PREFIX)/bin
	ln -sf $(SPNLDIR)/spinel $(PREFIX)/bin/spinel

uninstall:
	rm -f $(PREFIX)/bin/spinel
	rm -rf $(SPNLDIR)

# ---- Clean ----

clean:
	rm -rf build/
	rm -f spinel_parse spinel_codegen
