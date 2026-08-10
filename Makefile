# Spinel AOT Compiler - Makefile
#
# Usage:
#   make              Build the C compiler (runtime + spinel + tools)
#   make test         Run the feature tests (always a fresh run)
#   make bench        Run benchmarks vs CRuby
#   make optcarrot    End-to-end optcarrot integration test
#   make check        Fast pre-commit: rebuild + tests
#   make gate         Full pre-push: test || bench || optcarrot
#   make clean        Remove built binaries

# COPT: optimization level override. Default -O2 for release builds.
# Set `COPT=-O0 -g0` in config.mk for fast iteration / debugging.
# A command-line `make COPT=-O0` takes precedence.
COPT ?= -O2
# Machine-local overrides (gitignored config.mk is a common dev pattern).
-include config.mk

# Shared toolchain configuration (CC wrapping, CFLAGS, stamps, …).
include common.mk

# Prism library: prefer vendor/prism (fetched via `make deps`), then fall
# back to the Prism gem if one is installed. Override with PRISM_DIR=…
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

# rbs C parser. Fetched via `make deps` from rubygems.org. Consumed by
# spinel_rbs_extract to produce a seed file for the analyzer.
RBS_VERSION ?= 4.0.1
RBS_DIR      = vendor/rbs
RBS_INC      = $(RBS_DIR)/include
RBS_SRC      = $(wildcard $(RBS_DIR)/src/*.c) $(wildcard $(RBS_DIR)/src/util/*.c)
RBS_OBJ      = $(patsubst $(RBS_DIR)/src/%.c,build/rbs/%.o,$(RBS_SRC))
RBS_LIB      = build/librbs.a

.PHONY: all regexp rbs_extract rbs-test rbs-seed-test alloc-report-test rubyspec rubyspec-gate spin-check \
        test test-run clean-test-results regen-rbs-expected \
        regen-expected regen-expected-err bench optcarrot gate check gate-legs gate-test gate-bench \
        gate-optcarrot clean install uninstall deps tools

# `make all` includes the RBS extractor when vendor/rbs has been fetched
# (via `make deps`); without it the extractor is silently omitted. Built under
# build/ like other intermediates; rbs-seed-test copies it beside $(SPINEL),
# where main.c looks for it as a sibling at runtime.
RBS_EXTRACT_BIN = build/spinel_rbs_extract
ifneq ($(wildcard $(RBS_INC)/rbs/parser.h),)
  RBS_EXTRACT_TARGET = $(RBS_EXTRACT_BIN)
else
  RBS_EXTRACT_TARGET =
endif

# The single Spinel binary (compiler + cc driver). Defined here, before the
# `all` rule, because a rule's prerequisites are expanded as it is read.
# Built into bin/ alongside the companion tools (spinel-doctor, ...); bin/ sits
# beside lib/ so the binary resolves its runtime lib via ../lib, same as before.
SPINEL = bin/spinel

# Bundled carried-C spin packages (Path B). Defined here, before `all`, because
# GNU Make expands a rule's prerequisites immediately when the rule is read -- a
# definition further down would expand to empty in `all`'s prereq list. The
# build rule + rationale live further below (near the runtime archive).
BUNDLED_NATIVE_OBJS = packages/json/sp_json.o packages/stringio/sp_stringio.o packages/strscan/sp_strscan.o packages/base64/sp_base64.o
# Threaded variant of every bundled package object. A program that uses threads
# compiles its TU (and links the runtime archive) with -DSP_THREADS, which makes
# the runtime's per-worker globals thread-local; a package object built without
# it references them as non-TLS and the link fails (#3342). The driver picks the
# matching variant.
BUNDLED_NATIVE_MT_OBJS = $(BUNDLED_NATIVE_OBJS:.o=_mt.o)
PKG_MT_FLAGS = -DSP_THREADS -ftls-model=initial-exec

all: regexp $(SPINEL) $(RBS_EXTRACT_TARGET) tools $(BUNDLED_NATIVE_OBJS) $(BUNDLED_NATIVE_MT_OBJS)

# ---- Dependencies ----
deps: vendor/prism/include/prism/diagnostic.h vendor/rbs/include/rbs/parser.h

# Download the pre-built Prism gem from rubygems.org and extract its C
# sources (the .gem ships the generated headers — no rake/bundler needed).
vendor/prism/include/prism/diagnostic.h:
	@mkdir -p vendor/prism
	@echo "Fetching prism v$(PRISM_VERSION) from rubygems.org..."
	curl -sL -o /tmp/prism-$(PRISM_VERSION).gem https://rubygems.org/gems/prism-$(PRISM_VERSION).gem
	@tmpdir=$$(mktemp -d); \
	 tar -xf /tmp/prism-$(PRISM_VERSION).gem -C $$tmpdir data.tar.gz; \
	 tar -xzf $$tmpdir/data.tar.gz -C vendor/prism; \
	 rm -rf $$tmpdir /tmp/prism-$(PRISM_VERSION).gem
	@test -f $@ && echo "prism v$(PRISM_VERSION) ready at vendor/prism"

# Same shape: download the rbs gem and extract its bundled C parser.
vendor/rbs/include/rbs/parser.h:
	@mkdir -p vendor/rbs
	@echo "Fetching rbs v$(RBS_VERSION) from rubygems.org..."
	curl -sL -o /tmp/rbs-$(RBS_VERSION).gem https://rubygems.org/gems/rbs-$(RBS_VERSION).gem
	@tmpdir=$$(mktemp -d); \
	 tar -xf /tmp/rbs-$(RBS_VERSION).gem -C $$tmpdir data.tar.gz; \
	 tar -xzf $$tmpdir/data.tar.gz -C vendor/rbs; \
	 rm -rf $$tmpdir /tmp/rbs-$(RBS_VERSION).gem
	@test -f $@ && echo "rbs v$(RBS_VERSION) ready at vendor/rbs"

# If PRISM_DIR ended up empty (no vendor/prism, no gem), halt with a clear
# message before trying to build anything that needs it.
ifeq ($(PRISM_DIR),)
regexp all: prism-missing
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
	$(CC) -c $(COPT) -I$(PRISM_INC) -I$(PRISM_DIR)/src $< -o $@

# ---- rbs static library ----

build/librbs.a: $(RBS_OBJ)
	@mkdir -p build
	ar rcs $@ $^

build/rbs/%.o: $(RBS_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(COPT) -Wno-all -I$(RBS_INC) -I$(RBS_DIR)/src $< -o $@

# ---- C compiler (src/) ----
# The single-binary C reimplementation of the analyzer + code generator.
# Links src/spinel_parse.c, the library copy of the Prism walk (no main();
# exposes sp_parse_file_to_text).
# `spinel` is the single binary: it emits C and then drives cc to link it.
# (SPINEL itself is defined above, just before the `all` target.)

SPINEL_HDRS = src/node_table.h src/codegen.h src/codegen_internal.h src/types.h src/compiler.h src/analyze.h src/analyze_internal.h src/ffi_spec.h
SPINEL_OBJ  = build/csrc/node_table.o build/csrc/types.o build/csrc/compiler.o \
               build/csrc/ffi_spec.o \
               build/csrc/analyze.o build/csrc/analyze_util.o build/csrc/analyze_infer.o \
               build/csrc/analyze_scope.o build/csrc/analyze_pass.o build/csrc/codegen.o build/csrc/codegen_util.o \
               build/csrc/codegen_fold.o build/csrc/codegen_call.o build/csrc/codegen_call_recv.o build/csrc/codegen_iter.o \
               build/csrc/codegen_expr.o build/csrc/codegen_stmt.o build/csrc/main.o

build/csrc:
	@mkdir -p build/csrc

build/csrc/%.o: src/%.c $(SPINEL_HDRS) | build/csrc
	$(CC) $(CFLAGS) -Isrc -Ibuild/csrc -c $< -o $@

# Build revision, embedded in `spinel --version` (and spin's probe records).
# cmp-guarded so only a HEAD move recompiles main.o, not every build.
# The tmp name carries the PID: gate runs test/bench/optcarrot as parallel
# sub-makes, each re-evaluating this FORCE target -- a shared tmp name races
# (one job's rm strands the other's mv mid-flight).
build/csrc/spinel_rev.h: FORCE | build/csrc
	@r=$$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown); \
	t=$@.tmp.$$$$; \
	echo "#define SPINEL_BUILD_REV \"$$r\"" > $$t; \
	if cmp -s $$t $@; then rm -f $$t; else mv $$t $@; fi

build/csrc/main.o: build/csrc/spinel_rev.h

FORCE:

build/csrc/sp_parse_lib.o: src/spinel_parse.c $(PRISM_LIB) | build/csrc
	$(CC) $(CFLAGS) -I$(PRISM_INC) -c src/spinel_parse.c -o $@

$(SPINEL): $(SPINEL_OBJ) build/csrc/sp_parse_lib.o $(PRISM_LIB)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SPINEL_OBJ) build/csrc/sp_parse_lib.o $(PRISM_LIB) -lm $(LDFLAGS) -o $@
	@# Dev convenience: a repo-root `./spinel` pointing at the built binary
	@# (the installed command is `spinel` too). Best-effort; gitignored.
	@ln -sf $@ spinel 2>/dev/null || cp $@ spinel 2>/dev/null || true

# ---- RBS extractor ----
# Reads sig/**/*.rbs, emits the seed-file format spinel_analyze consumes
# when invoked with `spinel --rbs DIR`.

ifeq ($(wildcard $(RBS_INC)/rbs/parser.h),)
rbs_extract: rbs-missing
rbs-missing:
	@echo "Error: rbs C parser not found at $(RBS_INC)/rbs/parser.h."; \
	 echo "  Run 'make deps' to fetch it from rubygems.org into vendor/rbs."; \
	 exit 1
else
rbs_extract: $(RBS_EXTRACT_BIN)

$(RBS_EXTRACT_BIN): tools/spinel_rbs_extract.c $(RBS_LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(RBS_INC) tools/spinel_rbs_extract.c $(RBS_LIB) -o $@
endif

# ---- Runtime library (regexp + bigint + …) ----

RE_SRC = lib/regexp/re_compile.c lib/regexp/re_exec.c lib/regexp/re_utf8.c
RE_OBJ = $(patsubst lib/regexp/%.c,build/regexp/%.o,$(RE_SRC))

build/regexp/%.o: lib/regexp/%.c lib/regexp/re_internal.h
	@mkdir -p build/regexp
	$(CC) -c $(COPT) $(SEC_FLAGS) -Ilib/regexp $< -o $@

build/sp_bigint.o: lib/sp_bigint.c lib/sp_bigint.h lib/mruby_shim.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_bigint.c -o build/sp_bigint.o

build/sp_crypto.o: lib/sp_crypto.c lib/sp_crypto.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_crypto.c -o build/sp_crypto.o

build/sp_pack.o: lib/sp_pack.c lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_pack.c -o build/sp_pack.o


build/sp_time.o: lib/sp_time.c lib/sp_time.h lib/sp_alloc.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_time.c -o build/sp_time.o

build/sp_array.o: lib/sp_array.c lib/sp_array.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_random.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_array.c -o build/sp_array.o

build/sp_str.o: lib/sp_str.c lib/sp_str.h lib/sp_array.h lib/sp_alloc.h lib/sp_crypto.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_str.c -o build/sp_str.o

build/sp_exc.o: lib/sp_exc.c lib/sp_exc.h lib/sp_types.h lib/sp_gc.h lib/sp_alloc.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_exc.c -o build/sp_exc.o

build/sp_proc.o: lib/sp_proc.c lib/sp_proc.h lib/sp_types.h lib/sp_gc.h lib/sp_alloc.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_proc.c -o build/sp_proc.o

build/sp_hash.o: lib/sp_hash.c lib/sp_hash.h lib/sp_types.h lib/sp_gc.h lib/sp_array.h lib/sp_str.h lib/sp_inspect.h lib/sp_string.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_hash.c -o build/sp_hash.o

build/sp_re.o: lib/sp_re.c lib/sp_re.h lib/sp_array.h lib/sp_str.h lib/sp_string.h lib/sp_inspect.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_re.c -o build/sp_re.o

build/sp_core.o: lib/sp_core.c lib/sp_core.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_core.c -o build/sp_core.o

build/sp_system.o: lib/sp_system.c lib/sp_system.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_system.c -o build/sp_system.o

build/sp_gc.o: lib/sp_gc.c lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_gc.c -o build/sp_gc.o

build/sp_alloc.o: lib/sp_alloc.c lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_dtoa.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_alloc.c -o build/sp_alloc.o

build/sp_dtoa.o: lib/sp_dtoa.c lib/sp_dtoa.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_dtoa.c -o build/sp_dtoa.o

# Bundled carried-C spin packages (Path B): compiled standalone, NOT into the
# runtime archive, and linked on demand only when the program requires them
# (native_obj markers -> src/main.c). Package C compiles against the stable
# spinel/runtime.h ABI (-Ilib) plus its own package headers. BUNDLED_NATIVE_OBJS
# is defined near the top (before `all`, whose prereqs expand at parse time).
packages/json/sp_json.o: packages/json/sp_json.c packages/json/sp_json.h \
                         lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib -Ipackages/json packages/json/sp_json.c -o $@
packages/json/sp_json_mt.o: packages/json/sp_json.c packages/json/sp_json.h \
                            lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(PKG_MT_FLAGS) -Ilib -Ipackages/json packages/json/sp_json.c -o $@

build/sp_format.o: lib/sp_format.c lib/sp_format.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_format.c -o build/sp_format.o

# stringio is a native-bound spin package (Path B typed object): the struct,
# every method, and the header live in the package; the compiler knows it only
# through the native_* declarations in stringio.rb.
packages/stringio/sp_stringio.o: packages/stringio/sp_stringio.c packages/stringio/sp_stringio.h \
                                 lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib -Ipackages/stringio packages/stringio/sp_stringio.c -o $@
packages/stringio/sp_stringio_mt.o: packages/stringio/sp_stringio.c packages/stringio/sp_stringio.h \
                                    lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(PKG_MT_FLAGS) -Ilib -Ipackages/stringio packages/stringio/sp_stringio.c -o $@

# strscan is likewise a native-bound spin package; its regex matching links
# against the runtime archive's re_exec (a forward extern in the package C).
packages/strscan/sp_strscan.o: packages/strscan/sp_strscan.c \
                               lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib packages/strscan/sp_strscan.c -o $@
packages/strscan/sp_strscan_mt.o: packages/strscan/sp_strscan.c \
                                  lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(PKG_MT_FLAGS) -Ilib packages/strscan/sp_strscan.c -o $@

# base64 carries its whole implementation; digest carries none (it binds the
# runtime's vendored sp_crypto symbols and has no object of its own).
packages/base64/sp_base64.o: packages/base64/sp_base64.c \
                             lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib packages/base64/sp_base64.c -o $@
packages/base64/sp_base64_mt.o: packages/base64/sp_base64.c \
                                lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(PKG_MT_FLAGS) -Ilib packages/base64/sp_base64.c -o $@

build/sp_string.o: lib/sp_string.c lib/sp_string.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_string.c -o build/sp_string.o

build/sp_inspect.o: lib/sp_inspect.c lib/sp_inspect.h lib/sp_string.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_inspect.c -o build/sp_inspect.o

build/sp_marshal.o: lib/sp_marshal.c lib/sp_marshal.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_marshal.c -o build/sp_marshal.o

build/sp_fiber.o: lib/sp_fiber.c lib/sp_fiber.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_fiber.c -o build/sp_fiber.o

build/sp_sched.o: lib/sp_sched.c lib/sp_sched.h lib/sp_fiber.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_sched.c -o build/sp_sched.o

build/sp_net.o: lib/sp_net.c lib/sp_net.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_net.c -o build/sp_net.o

build/sp_io.o: lib/sp_io.c lib/sp_io.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_io.c -o build/sp_io.o

build/sp_random.o: lib/sp_random.c lib/sp_random.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib lib/sp_random.c -o build/sp_random.o

build/sp_cold.o: lib/sp_cold.c $(RT_HDRS)
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib -Ilib/regexp lib/sp_cold.c -o build/sp_cold.o

SP_RT_LIB = lib/libspinel_rt.a

RT_MEMBERS = sp_bigint sp_crypto sp_pack sp_time sp_core sp_net sp_system sp_gc sp_alloc sp_dtoa sp_marshal sp_format sp_string sp_inspect sp_array sp_str sp_hash sp_proc sp_exc sp_re sp_random sp_fiber sp_sched sp_io sp_cold

$(SP_RT_LIB): $(RE_OBJ) $(addprefix build/,$(addsuffix .o,$(RT_MEMBERS)))
	ar rcs $@ $^

# ---- Threaded runtime variant (-DSP_THREADS) ----
# Same sources, compiled with -DSP_THREADS, linked when the program uses
# Thread/Mutex/Queue/... (see codegen's SPINEL_USES_THREADS marker and the
# archive selection in src/main.c). Phase 0 has no SP_THREADS #ifdefs yet, so it
# is functionally identical today; it is the linchpin for Phase 1 parallelism
# (per-worker __thread GC state, locks, real OS workers) without touching the
# byte-identical single-threaded path. -ftls-model=initial-exec keeps the
# eventual per-worker TLS reads a single segment-relative load.
SP_RT_MT_LIB = lib/libspinel_rt_mt.a
MT_DEF = -DSP_THREADS -ftls-model=initial-exec
RT_HDRS = $(wildcard lib/*.h lib/regexp/*.h)

# Specific rule before generic: GNU Make 3.81 (macOS system make) picks the
# first matching pattern rule, not the shortest-stem one (3.82+).
build/mt/regexp/%.o: lib/regexp/%.c lib/regexp/re_internal.h
	@mkdir -p $(@D)
	$(CC) -c $(COPT) $(SEC_FLAGS) $(MT_DEF) -Ilib/regexp $< -o $@

build/mt/%.o: lib/%.c $(RT_HDRS)
	@mkdir -p $(@D)
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(MT_DEF) -Ilib -Ilib/regexp $< -o $@

RE_MT_OBJ = $(patsubst lib/regexp/%.c,build/mt/regexp/%.o,$(RE_SRC))

$(SP_RT_MT_LIB): $(RE_MT_OBJ) $(addprefix build/mt/,$(addsuffix .o,$(RT_MEMBERS)))
	ar rcs $@ $^

# ---- ThreadSanitizer build of the threaded runtime (Phase 1 validation) ----
# The single-threaded gate links the plain archive even for threaded tests
# (test-run does its own cc), so it never exercises the mt archive's parallel
# paths. This TSan-instrumented mt archive is the race-checking gate for the
# real-parallelism work: build a threaded program against it (see scripts/
# tsan-run.sh) and run -- TSan flags any data race on the shared GC heap, the
# thread registry, or the run queue. Not built by default (TSan slows the build
# and the binary); `make tsan-archive` on demand.
SP_RT_MT_TSAN_LIB = lib/libspinel_rt_mt_tsan.a
TSAN_DEF = $(MT_DEF) -fsanitize=thread -g

# Specific before generic, as in the mt pair above.
build/mt-tsan/regexp/%.o: lib/regexp/%.c lib/regexp/re_internal.h
	@mkdir -p $(@D)
	$(CC) -c -O1 $(SEC_FLAGS) $(TSAN_DEF) -Ilib/regexp $< -o $@

build/mt-tsan/%.o: lib/%.c $(RT_HDRS)
	@mkdir -p $(@D)
	$(CC) -c -O1 -Wno-all $(SEC_FLAGS) $(TSAN_DEF) -Ilib -Ilib/regexp $< -o $@

RE_MT_TSAN_OBJ = $(patsubst lib/regexp/%.c,build/mt-tsan/regexp/%.o,$(RE_SRC))

$(SP_RT_MT_TSAN_LIB): $(RE_MT_TSAN_OBJ) $(addprefix build/mt-tsan/,$(addsuffix .o,$(RT_MEMBERS)))
	ar rcs $@ $^

tsan-archive: $(SP_RT_MT_TSAN_LIB)

regexp: $(SP_RT_LIB) $(SP_RT_MT_LIB)

# ---- In-tree developer tools ----

# spinel-doctor / spinel-reduce / spinel-flatten: written in the spinel subset
# and compiled by spinel itself (dogfood), so their only runtime dependency is
# cc — the same as the compiler. Each tools/<name>.rb becomes bin/spinel-<name>,
# beside the compiler, so the `spinel-<name>` command is found next to `spinel`.
# A tool that no longer fits the subset breaks the build, which keeps them honest.
TOOL_NAMES = doctor reduce flatten
TOOL_BINS  = $(addprefix bin/spinel-,$(TOOL_NAMES))

tools: $(TOOL_BINS) bin/spin

# spin: the project tool (self-hosted; see docs/spin.md). Stages the RBS
# extractor beside the compiler when vendor/rbs is fetched: a spin-driven
# build resolves --rbs via <dir-of-spinel>/spinel_rbs_extract, and without
# the copy it silently lost every .rbs seed (#1845 bounce 6).
bin/spin: tools/spin.rb tools/spin/toml.rb $(SPINEL) $(SP_RT_LIB) $(RBS_EXTRACT_TARGET)
	$(SPINEL) tools/spin.rb -o bin/spin
	@if [ -n "$(RBS_EXTRACT_TARGET)" ]; then \
	  cp -f $(RBS_EXTRACT_BIN) bin/spinel_rbs_extract; \
	  echo "$(RBS_EXTRACT_BIN) -> bin/spinel_rbs_extract"; \
	fi

bin/spinel-%: tools/%.rb tools/tool_common.rb $(SPINEL) $(SP_RT_LIB)
	@mkdir -p bin
	$(SPINEL) $< -o $@

# ---- Test ----

TESTS := $(wildcard test/*.rb)
# Mode-incompatible: int_overflow_raises pins raise-mode semantics; under
# --int-overflow=promote the same code auto-promotes and output diverges.
ifeq ($(SPINEL_INT_OVERFLOW),promote)
TESTS := $(filter-out test/int_overflow_raises.rb,$(TESTS))
# Drive the spinel front-end and the C compile in promote mode so the test
# rule actually exercises the auto-promotion path end to end.
SP_OV_FLAG := --int-overflow=promote
SP_OV_DEFINE := -DSP_INT_OVERFLOW_MODE_PROMOTE
else
# `promote_*` tests overflow on purpose and only have defined output under
# --int-overflow=promote; in raise/wrap mode they would (correctly) raise.
TESTS := $(filter-out test/promote_%.rb,$(TESTS))
endif
TEST_TARGETS := $(patsubst test/%.rb,build/test-results/%.ok,$(TESTS))

# Bundled spin packages carry their own test/*.rb (the same snapshot contract,
# runnable with `spin test` inside the package). The compiler gate runs them
# too -- bundled packages are versioned with the compiler, so a compiler change
# that breaks one must fail here, not at package-publish time. Targets are
# namespaced pkg.<package>.<test>.ok to avoid colliding with test/ names.
PKG_TESTS := $(wildcard packages/*/test/*.rb)
pkg_of = $(word 2,$(subst /, ,$(1)))
PKG_TEST_TARGETS := $(foreach t,$(PKG_TESTS),build/test-results/pkg.$(call pkg_of,$(t)).$(notdir $(t:.rb=)).ok)

# Warnings the generated-C -Werror check should not gate on. clang enables
# -Wunused-value by default (gcc only under -Wall, which the build disables),
# so a discarded value-producing statement-expression -- e.g. the
# `({ ...; v; })` emitted for `Fiber[:k] = v` in statement position -- fails
# CI under clang while gcc is silent. The value is intentionally discarded;
# behaviour is still gated by the output diff. Keep this list minimal.
TEST_WARN_SUPPRESS := -Wno-unused-value

# The main suite compiles every generated TU with -Werror, so a pointer-type
# mismatch in emitted C fails a test. The --rbs fixtures did not: they build
# with a plain cc, which is why the one family that needs an RBS signature to
# arise -- a subclass instance in a slot the signature pins to an ancestor --
# could reach a release without any host reporting it (#3418). Only this
# diagnostic is promoted, not -Werror wholesale: these fixtures deliberately
# exercise shapes that warn for other, expected reasons.
RBS_SEED_STRICT := -Werror=incompatible-pointer-types

# ---- Precompiled runtime header for the per-test compiles ----
# Every generated test TU includes the same lib/spinel_rt.h; the cc step is
# >99% of a test's cost and roughly half of that is parsing the header, so
# the suite precompiles it once per make invocation (measured: gcc -15%,
# clang -23% on the per-test compile). Two variants cover the two TU shapes
# the emitter produces: plain, and `#define SP_TU_NO_POLY_RENDER 1` before
# the include. gcc picks the .gch up implicitly from the include path; clang
# ignores gcc-style implicit lookup and needs an explicit -include-pch.
# Each variant dir also carries a copy of spinel_rt.h so gcc degrades to a
# normal textual include if the .gch is unusable. The PCH path is keyed on
# compiler kind and $(OPT) because a PCH only loads under the exact flags
# it was built with (for clang a mismatch is a hard error, not a fallback).
CC_KIND  := $(if $(findstring clang,$(shell $(CC) --version 2>/dev/null | head -1)),clang,gcc)
PCH_ROOT := build/pch/$(CC_KIND)$(subst -,,$(OPT))
PCH_FLAGS = $(CFLAGS) $(SP_OV_DEFINE) -Werror $(TEST_WARN_SUPPRESS) $(SEC_FLAGS)
PCH_PLAIN  := $(PCH_ROOT)/plain/spinel_rt.h.gch
PCH_NOPOLY := $(PCH_ROOT)/nopoly/spinel_rt.h.gch
SP_LIB_HDRS := $(wildcard lib/*.h)

$(PCH_PLAIN): $(SP_LIB_HDRS)
	@mkdir -p $(@D)
	@cp lib/spinel_rt.h $(@D)/spinel_rt.h
	@$(CC) $(PCH_FLAGS) -Ilib -x c-header $(@D)/spinel_rt.h -o $@

$(PCH_NOPOLY): $(SP_LIB_HDRS)
	@mkdir -p $(@D)
	@cp lib/spinel_rt.h $(@D)/spinel_rt.h
	@$(CC) $(PCH_FLAGS) -DSP_TU_NO_POLY_RENDER=1 -Ilib -x c-header $(@D)/spinel_rt.h -o $@

ifeq ($(CC_KIND),clang)
PCH_USE_PLAIN  = -include-pch $(PCH_PLAIN)
PCH_USE_NOPOLY = -include-pch $(PCH_NOPOLY)
# clang tests compile+link in ONE driver invocation: sccache declines to
# cache -include-pch compiles, so the split buys nothing there, and the
# extra ~2000 driver spawns are expensive on macOS (process launch cost).
# gcc keeps the split: its separate compile step is sccache-cacheable.
TEST_SINGLE_INVOKE := 1
else
PCH_USE_PLAIN  = -I$(PCH_ROOT)/plain
PCH_USE_NOPOLY = -I$(PCH_ROOT)/nopoly
TEST_SINGLE_INVOKE :=
endif

# Host CPU count (Linux nproc, macOS sysctl), a safe fallback of 4.
NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
# Default the standalone suite to parallel: inject -j<nproc> ONLY when the
# invocation supplied no job count of its own. Under `make -j gate` the leg
# already inherits the jobserver (a -j is in MAKEFLAGS), so this stays empty and
# there is no oversubscription; a bare `make test` / `make check` now fans the
# per-test .ok builds across the cores (~5x). An explicit `make -jN test` wins.
TEST_JOBS := $(if $(filter -j%,$(MAKEFLAGS)),,-j$(NPROC))
# `bench` is one monolithic recipe (a shell loop), so it can't fan out through
# the .ok pattern the way `test` does. Parallelize its loop with `xargs -P`
# instead -- but only when there is no jobserver to respect: under `make -j gate`
# the leg runs alongside test/optcarrot on the shared jobserver, so keep it
# serial (-P 1) there and let the jobserver schedule; a bare `make bench` uses
# all cores.
BENCH_PJOBS := $(if $(filter -j%,$(MAKEFLAGS)),1,$(NPROC))

# `make test` always runs fresh: it wipes the prior `.ok` stamps first,
# then runs the suite. (The old incremental `test` + `retest` split is
# gone — a stale `.ok` reading PASS was a recurring foot-gun.)
test:
	+@$(MAKE) --no-print-directory clean-test-results
	+@$(MAKE) $(TEST_JOBS) --no-print-directory test-run

# The actual run. rbs-test golden-checks the RBS extractor (cheap, C-only).
# rbs-seed-test checks the seeds actually reach the analyzer (incl. nested
# classes, #1417).
test-run: rbs-test rbs-seed-test $(TEST_TARGETS) $(PKG_TEST_TARGETS)
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

# ---- RBS extractor golden tests ----
RBS_TEST_SRCS := $(sort $(wildcard test/rbs/*.rbs))

ifeq ($(wildcard $(RBS_INC)/rbs/parser.h),)
rbs-test:
	@echo "rbs-test: skipped (vendor/rbs not fetched; run 'make deps')"
regen-rbs-expected:
	@echo "regen-rbs-expected: skipped (vendor/rbs not fetched; run 'make deps')"
else
rbs-test: $(RBS_EXTRACT_BIN)
	@fail=0; n=0; \
	for f in $(RBS_TEST_SRCS); do \
	  n=$$((n+1)); \
	  exp="$${f%.rbs}.seed.expected"; \
	  if [ ! -f "$$exp" ]; then echo "rbs-test: MISSING golden $$exp"; fail=1; continue; fi; \
	  d=$$($(RBS_EXTRACT_BIN) "$$f" 2>/dev/null | diff -u "$$exp" - 2>&1); \
	  if [ -z "$$d" ]; then \
	    if [ -t 1 ]; then printf .; fi; \
	  else \
	    echo; echo "rbs-test FAIL: $$f"; echo "$$d"; fail=1; \
	  fi; \
	done; \
	if [ -t 1 ]; then printf '\n'; fi; \
	if [ $$fail -ne 0 ]; then echo "RBS extractor tests: FAIL"; exit 1; fi; \
	echo "RBS extractor tests: $$n pass"

regen-rbs-expected: $(RBS_EXTRACT_BIN)
	@for f in $(RBS_TEST_SRCS); do \
	  $(RBS_EXTRACT_BIN) "$$f" > "$${f%.rbs}.seed.expected"; \
	  echo "regen: $${f%.rbs}.seed.expected"; \
	done
endif

# End-to-end --rbs seeding check (#1417). The extractor emits a module-nested
# class under its qualified name (`Outer_Box`), but the compiler's class table
# stores the leaf name (`Box`) + enclosing_class. seed_class_index must match
# the two so the seed reaches the class. The fixture's `@label` is declared
# `String?` but only ever assigned nil, so inference alone leaves it poly --
# only an applied seed pins it to a `const char *` field. The extractor must sit
# beside $(SPINEL) (main.c looks for it as a sibling), so copy it there first.
ifeq ($(wildcard $(RBS_INC)/rbs/parser.h),)
rbs-seed-test:
	@echo "rbs-seed-test: skipped (vendor/rbs not fetched; run 'make deps')"
else
rbs-seed-test: $(SPINEL) $(RBS_EXTRACT_BIN) $(SP_RT_LIB)
	@cp -f $(RBS_EXTRACT_BIN) $(dir $(SPINEL))spinel_rbs_extract
	@tmp=$$(mktemp -d /tmp/spinel-rbsseed.XXXXXX); ok=1; \
	$(SPINEL) test/rbs-seed/nested_ivar.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/out.c" 2>/dev/null; \
	grep -Eq 'const char[[:space:]]+\*[[:space:]]*iv_label' "$$tmp/out.c" || { echo "rbs-seed-test: FAIL (#1417: module-nested-class seed not applied)"; ok=0; }; \
	if grep -Eq 'sp_RbVal[[:space:]]+iv_label' "$$tmp/out.c"; then echo "rbs-seed-test: FAIL (#1417: ivar stayed poly)"; ok=0; fi; \
	$(CC) -fsyntax-only -Ilib "$$tmp/out.c" 2>/dev/null || { echo "rbs-seed-test: FAIL (nested_ivar C invalid)"; ok=0; }; \
	$(SPINEL) test/rbs-seed/boundary.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/b.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/b.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/b" 2>"$$tmp/b.err"; then \
	  "$$tmp/b" > "$$tmp/b.out" 2>/dev/null; \
	  cmp -s "$$tmp/b.out" test/rbs-seed/boundary.expected || { echo "rbs-seed-test: FAIL (#1417 boundary output mismatch)"; diff -u test/rbs-seed/boundary.expected "$$tmp/b.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#1417 boundary coercion C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/module_clone_divergent.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/mc.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/mc.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/mc" 2>"$$tmp/mc.err"; then \
	  "$$tmp/mc" > "$$tmp/mc.out" 2>/dev/null; \
	  cmp -s "$$tmp/mc.out" test/rbs-seed/module_clone_divergent.expected || { echo "rbs-seed-test: FAIL (#2008 module-clone divergent-hash output mismatch)"; diff -u test/rbs-seed/module_clone_divergent.expected "$$tmp/mc.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#2008 module-clone divergent-hash C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/colliding_class_pin.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/cp.c" 2>/dev/null; \
	grep -Eq 'const char[[:space:]]*\*[[:space:]]*iv_rtag' "$$tmp/cp.c" || { echo "rbs-seed-test: FAIL (collision-renamed class seed not applied)"; ok=0; }; \
	grep -Eq 'sp_RbVal[[:space:]]+sp_Blue__Base_btag' "$$tmp/cp.c" || { echo "rbs-seed-test: FAIL (poly union return seed not pinned)"; ok=0; }; \
	grep -Eq 'const char[[:space:]]*\*[[:space:]]*iv_itag' "$$tmp/cp.c" || { echo "rbs-seed-test: FAIL (seed for class nested in a renamed class not applied)"; ok=0; }; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/cp.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/cp" 2>"$$tmp/cp.err"; then \
	  "$$tmp/cp" > "$$tmp/cp.out" 2>/dev/null; \
	  cmp -s "$$tmp/cp.out" test/rbs-seed/colliding_class_pin.expected || { echo "rbs-seed-test: FAIL (colliding_class_pin output mismatch)"; diff -u test/rbs-seed/colliding_class_pin.expected "$$tmp/cp.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (colliding_class_pin C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/nilable_scalar_hash_key.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/nk.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/nk.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/nk" 2>"$$tmp/nk.err"; then \
	  "$$tmp/nk" > "$$tmp/nk.out" 2>/dev/null; \
	  cmp -s "$$tmp/nk.out" test/rbs-seed/nilable_scalar_hash_key.expected || { echo "rbs-seed-test: FAIL (a nilable scalar seed's nil is a different Hash key than a literal nil)"; diff -u test/rbs-seed/nilable_scalar_hash_key.expected "$$tmp/nk.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (nilable_scalar_hash_key C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/void_block_tail.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/v.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/v.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/v" 2>"$$tmp/v.err"; then \
	  "$$tmp/v" > "$$tmp/v.out" 2>/dev/null; \
	  cmp -s "$$tmp/v.out" test/rbs-seed/void_block_tail.expected || { echo "rbs-seed-test: FAIL (void block tail output mismatch)"; diff -u test/rbs-seed/void_block_tail.expected "$$tmp/v.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (void-returning call as proc tail: C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/map_untyped_poly.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/mu.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/mu.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/mu" 2>"$$tmp/mu.err"; then \
	  "$$tmp/mu" > "$$tmp/mu.out" 2>/dev/null; \
	  cmp -s "$$tmp/mu.out" test/rbs-seed/map_untyped_poly.expected || { echo "rbs-seed-test: FAIL (untyped map-into-poly output mismatch)"; diff -u test/rbs-seed/map_untyped_poly.expected "$$tmp/mu.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (untyped map result boxed as sp_box_int: C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/int_grows_bignum.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/ig.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/ig.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/ig" 2>"$$tmp/ig.err"; then \
	  "$$tmp/ig" > "$$tmp/ig.out" 2>/dev/null; \
	  cmp -s "$$tmp/ig.out" test/rbs-seed/int_grows_bignum.expected || { echo "rbs-seed-test: FAIL (an RBS Integer return truncated a bignum body)"; diff -u test/rbs-seed/int_grows_bignum.expected "$$tmp/ig.out" | head -20; ok=0; }; \
	else echo "rbs-seed-test: FAIL (int_grows_bignum C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/capture_civ_array.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/cca.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/cca.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/cca" 2>"$$tmp/cca.err"; then \
	  "$$tmp/cca" > "$$tmp/cca.out" 2>/dev/null; \
	  cmp -s "$$tmp/cca.out" test/rbs-seed/capture_civ_array.expected || { echo "rbs-seed-test: FAIL (#1827 typed-array return pin output mismatch)"; diff -u test/rbs-seed/capture_civ_array.expected "$$tmp/cca.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#1827 Array[String] return pin: C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/poly_array_ivar.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/pa.c" 2>/dev/null; \
	grep -Eq 'sp_PolyArray[[:space:]]*\*[[:space:]]*iv_kids' "$$tmp/pa.c" || { echo "rbs-seed-test: FAIL (poly_array ivar seed dropped)"; ok=0; }; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/pa.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/pa" 2>"$$tmp/pa.err"; then \
	  "$$tmp/pa" > "$$tmp/pa.out" 2>/dev/null; \
	  cmp -s "$$tmp/pa.out" test/rbs-seed/poly_array_ivar.expected || { echo "rbs-seed-test: FAIL (poly_array ivar output mismatch)"; ok=0; }; \
	else echo "rbs-seed-test: FAIL (poly_array ivar: C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/pinned_container.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/pc.c" 2>/dev/null; \
	grep -Eq 'sp_PolyArray[[:space:]]*\*[[:space:]]*iv_kids' "$$tmp/pc.c" || { echo "rbs-seed-test: FAIL (ivar seed pin lost to fixpoint inference)"; ok=0; }; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/pc.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/pc" 2>"$$tmp/pc.err"; then \
	  "$$tmp/pc" > "$$tmp/pc.out" 2>/dev/null; \
	  cmp -s "$$tmp/pc.out" test/rbs-seed/pinned_container.expected || { echo "rbs-seed-test: FAIL (pinned container output mismatch)"; diff -u test/rbs-seed/pinned_container.expected "$$tmp/pc.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (pinned container: C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/nilable_arg_group_by.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/gb.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/gb.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/gb" 2>"$$tmp/gb.err"; then \
	  "$$tmp/gb" > "$$tmp/gb.out" 2>/dev/null; \
	  cmp -s "$$tmp/gb.out" test/rbs-seed/nilable_arg_group_by.expected || { echo "rbs-seed-test: FAIL (#2438 nilable-arg group_by output mismatch)"; diff -u test/rbs-seed/nilable_arg_group_by.expected "$$tmp/gb.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#2438 nilable-arg group_by: C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/inherited_pin_conflict.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/ipc.c" 2>"$$tmp/ipc.warn"; \
	grep -q "ivar pin @id dropped on Thing" "$$tmp/ipc.warn" || { echo "rbs-seed-test: FAIL (#1871 conflicting inherited pin didn't warn)"; ok=0; }; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/ipc.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/ipc" 2>"$$tmp/ipc.err"; then \
	  "$$tmp/ipc" > "$$tmp/ipc.out" 2>/dev/null; \
	  cmp -s "$$tmp/ipc.out" test/rbs-seed/inherited_pin_conflict.expected || { echo "rbs-seed-test: FAIL (#1871 inherited-pin output mismatch)"; diff -u test/rbs-seed/inherited_pin_conflict.expected "$$tmp/ipc.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#1871 inherited pin conflict: C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/override_family_ret.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/ofr.c" 2>/dev/null; \
	$(CC) -fsyntax-only -Ilib "$$tmp/ofr.c" 2>/dev/null || { echo "rbs-seed-test: FAIL (#3203 override-family return seed split decl/call-site repr)"; ok=0; }; \
	$(SPINEL) test/rbs-seed/untyped_array_ret.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/ua.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/ua.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/ua" 2>"$$tmp/ua.err"; then \
	  "$$tmp/ua" > "$$tmp/ua.out" 2>/dev/null; \
	  cmp -s "$$tmp/ua.out" test/rbs-seed/untyped_array_ret.expected || { echo "rbs-seed-test: FAIL (#3279 untyped-array return output mismatch)"; diff -u test/rbs-seed/untyped_array_ret.expected "$$tmp/ua.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#3279 untyped-array return: C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/yield_union_hash_obj.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/yu.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/yu.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/yu" 2>"$$tmp/yu.err"; then \
	  "$$tmp/yu" > "$$tmp/yu.out" 2>/dev/null; \
	  cmp -s "$$tmp/yu.out" test/rbs-seed/yield_union_hash_obj.expected || { echo "rbs-seed-test: FAIL (#3278 yield-union output mismatch)"; diff -u test/rbs-seed/yield_union_hash_obj.expected "$$tmp/yu.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#3278 yield-union: C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/nilable_scalar_ivar.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/ns.c" 2>/dev/null; \
	grep -Eq 'sp_RbVal[[:space:]]+iv_f;' "$$tmp/ns.c" || { echo "rbs-seed-test: FAIL (#3412: bool? pinned a slot with no nil)"; ok=0; }; \
	grep -Eq 'sp_RbVal[[:space:]]+iv_y;' "$$tmp/ns.c" || { echo "rbs-seed-test: FAIL (#3412: Symbol? pinned a slot with no nil)"; ok=0; }; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/ns.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/ns" 2>"$$tmp/ns.err"; then \
	  "$$tmp/ns" > "$$tmp/ns.out" 2>/dev/null; \
	  cmp -s "$$tmp/ns.out" test/rbs-seed/nilable_scalar_ivar.expected || { echo "rbs-seed-test: FAIL (#3412 nilable-scalar ivar output mismatch)"; diff -u test/rbs-seed/nilable_scalar_ivar.expected "$$tmp/ns.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#3412 nilable-scalar ivar: C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/nilable_scalar_ret.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/nr.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/nr.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/nr" 2>"$$tmp/nr.err"; then \
	  "$$tmp/nr" > "$$tmp/nr.out" 2>/dev/null; \
	  cmp -s "$$tmp/nr.out" test/rbs-seed/nilable_scalar_ret.expected || { echo "rbs-seed-test: FAIL (#3458 nilable-scalar return output mismatch)"; diff -u test/rbs-seed/nilable_scalar_ret.expected "$$tmp/nr.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#3458 nilable-scalar return: C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/nilable_scalar_arg.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/na.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/na.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/na" 2>"$$tmp/na.err"; then \
	  "$$tmp/na" > "$$tmp/na.out" 2>/dev/null; \
	  cmp -s "$$tmp/na.out" test/rbs-seed/nilable_scalar_arg.expected || { echo "rbs-seed-test: FAIL (#3465 nilable-scalar arg output mismatch)"; diff -u test/rbs-seed/nilable_scalar_arg.expected "$$tmp/na.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#3465 nilable-scalar arg: C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/subclass_into_ancestor_slot.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/sa.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib -Werror=incompatible-pointer-types "$$tmp/sa.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/sa" 2>"$$tmp/sa.err"; then \
	  "$$tmp/sa" > "$$tmp/sa.out" 2>/dev/null; \
	  cmp -s "$$tmp/sa.out" test/rbs-seed/subclass_into_ancestor_slot.expected || { echo "rbs-seed-test: FAIL (#3418 ancestor-slot output mismatch)"; diff -u test/rbs-seed/subclass_into_ancestor_slot.expected "$$tmp/sa.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#3418: emitted C is not pointer-typeclean -- GCC 14+ rejects it outright)"; sed -n 1,20p "$$tmp/sa.err"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/seed_check.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/sk.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) -DSP_RBS_CHECK "$$tmp/sk.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/sk" 2>"$$tmp/sk.err"; then \
	  "$$tmp/sk" > "$$tmp/sk.out" 2>&1; \
	  cmp -s "$$tmp/sk.out" test/rbs-seed/seed_check.expected || { echo "rbs-seed-test: FAIL (seed check fired on an honest seed)"; diff -u test/rbs-seed/seed_check.expected "$$tmp/sk.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (seed_check: C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/seed_check_bad.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/skb.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) -DSP_RBS_CHECK "$$tmp/skb.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/skb" 2>"$$tmp/skb.err"; then \
	  if "$$tmp/skb" > "$$tmp/skb.out" 2>&1; then echo "rbs-seed-test: FAIL (a contradicted seed did NOT abort under -DSP_RBS_CHECK)"; ok=0; \
	  else grep -q "seed violated" "$$tmp/skb.out" || { echo "rbs-seed-test: FAIL (contradicted seed aborted without naming the seed)"; sed -n 1,5p "$$tmp/skb.out"; ok=0; }; fi; \
	else echo "rbs-seed-test: FAIL (seed_check_bad: C did not compile)"; ok=0; fi; \
	if $(SPINEL) test/rbs-seed/seed_contradiction.rb --rbs test/rbs-seed/sig \
	     -c --no-line-map -o "$$tmp/sx.c" >"$$tmp/sx.out" 2>&1; then \
	  echo "rbs-seed-test: FAIL (a statically contradicted seed compiled)"; ok=0; \
	else grep -q "seed contradicted" "$$tmp/sx.out" || { echo "rbs-seed-test: FAIL (contradicted seed rejected without saying why)"; sed -n 1,5p "$$tmp/sx.out"; ok=0; }; fi; \
	if $(SPINEL) test/rbs-seed/seed_contradiction_arg.rb --rbs test/rbs-seed/sig \
	     -c --no-line-map -o "$$tmp/sxa.c" >"$$tmp/sxa.out" 2>&1; then \
	  echo "rbs-seed-test: FAIL (a contradicted seed on an ARGUMENT compiled)"; ok=0; \
	else grep -q "seed contradicted" "$$tmp/sxa.out" || { echo "rbs-seed-test: FAIL (contradicted argument rejected without saying why)"; sed -n 1,5p "$$tmp/sxa.out"; ok=0; }; fi; \
	for t in hash_kind_widened_return memoized_ivar_hash_ret poly_dispatch_arm_arg_type nilable_scalar_yield_key nilable_scalar_deep_chain nilable_scalar_paths poly_index_hash_dispatch yield_site_scalar_tail poly_container_op_result; do \
	  $(SPINEL) test/rbs-seed/$$t.rb --rbs test/rbs-seed/sig -c --no-line-map -o "$$tmp/$$t.c" 2>/dev/null; \
	  if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/$$t.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/$$t" 2>"$$tmp/$$t.err"; then \
	    "$$tmp/$$t" > "$$tmp/$$t.out" 2>/dev/null; \
	    cmp -s "$$tmp/$$t.out" test/rbs-seed/$$t.expected || { echo "rbs-seed-test: FAIL ($$t output mismatch)"; diff -u test/rbs-seed/$$t.expected "$$tmp/$$t.out" || true; ok=0; }; \
	  else echo "rbs-seed-test: FAIL ($$t: C did not compile)"; sed -n 1,10p "$$tmp/$$t.err"; ok=0; fi; \
	done; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "rbs-seed-test: pass"; else exit 1; fi
endif

# The .ok target is the test's stamp. Order-only $(SPINEL) keeps a
# compiler relink from invalidating every test.
# One snapshot test: compile $< with the integrated pipeline, run, diff
# against $<.expected (or CRuby), write PASS/FAIL/ERR to $@. Shared by the
# test/ rule and the per-package rules below.
# The generated C goes to a stable path derived from $@ (not the per-test
# tmpdir): the compile hash sccache computes covers the preprocessed source,
# which embeds the input path in #line directives, so a random tmpdir path
# would give every run a fresh cache key and the cache would never hit.
# The .c/.o are deleted after the link -- only the cache entry survives.
define RUN_ONE_TEST
@mkdir -p build/test-results
@tmpdir=$$(mktemp -d /tmp/spinel-test.XXXXXX); \
ast=$$tmpdir/test.ast; \
ir=$$tmpdir/test.ir; \
cfile=$(@:.ok=.c); \
bin=$$tmpdir/test_bin; \
exp=$$tmpdir/expected; \
act=$$tmpdir/actual; \
experr=$$tmpdir/experr; \
acterr=$$tmpdir/acterr; \
args=""; \
if [ -f "$<.args" ]; then args=$$(cat "$<.args"); fi; \
stdinf=/dev/null; \
if [ -f "$<.stdin" ]; then stdinf="$<.stdin"; fi; \
rm -f "$@.diff"; \
$(SPINEL) "$<" $(SP_OV_FLAG) -c --no-line-map -o "$$cfile" 2>/dev/null && \
{ pchuse="$(PCH_USE_PLAIN)"; pchf="$(PCH_PLAIN)"; \
  if head -2 "$$cfile" | grep -q SP_TU_NO_POLY_RENDER; then pchuse="$(PCH_USE_NOPOLY)"; pchf="$(PCH_NOPOLY)"; fi; \
  [ -f "$$pchf" ] || pchuse=""; \
  if [ -n "$(TEST_SINGLE_INVOKE)" ]; then \
    $(CC) $(CFLAGS) $(SP_OV_DEFINE) -Werror $(TEST_WARN_SUPPRESS) $(SEC_FLAGS) $$pchuse -Ilib "$$cfile" $(BUNDLED_NATIVE_OBJS) $(SP_RT_LIB) $(LDFLAGS) -lm $(GC_FLAGS) -o "$$bin" 2>/dev/null; \
  else \
    $(CC) $(CFLAGS) $(SP_OV_DEFINE) -Werror $(TEST_WARN_SUPPRESS) $(SEC_FLAGS) $$pchuse -Ilib -c "$$cfile" -o "$$cfile.o" 2>/dev/null && \
    $(CC) $(CFLAGS) "$$cfile.o" $(BUNDLED_NATIVE_OBJS) $(SP_RT_LIB) $(LDFLAGS) -lm $(GC_FLAGS) -o "$$bin" 2>/dev/null; \
  fi; }; \
if [ $$? -eq 0 ]; then \
  if [ -f "$<.expected" ]; then \
    LC_ALL=C sed 's/\r$$//' "$<.expected" >"$$exp.n"; \
  else \
    $(TIMEOUT10) $(REF_RUBY) "$<" $$args <"$$stdinf" >"$$exp" 2>/dev/null; \
    ruby_rc=$$?; \
    if [ $$ruby_rc -ne 0 ] && [ "$(REF_RUBY)" != "ruby" ]; then \
      $(TIMEOUT10) ruby "$<" $$args <"$$stdinf" >"$$exp" 2>/dev/null; \
    fi; \
    LC_ALL=C sed 's/\r$$//' "$$exp" >"$$exp.n"; \
  fi; \
  $(TIMEOUT10) "$$bin" $$args <"$$stdinf" >"$$act" 2>"$$acterr"; \
  LC_ALL=C sed 's/\r$$//' "$$act" >"$$act.n"; \
  LC_ALL=C sed 's/\r$$//' "$$acterr" >"$$acterr.n"; \
  if [ -f "$<.err.expected" ]; then \
    LC_ALL=C sed 's/\r$$//' "$<.err.expected" >"$$experr.n"; \
  else \
    : > "$$experr.n"; \
  fi; \
  if cmp -s "$$exp.n" "$$act.n" && cmp -s "$$experr.n" "$$acterr.n"; then \
    echo PASS > "$@"; \
    if [ -t 1 ]; then printf .; fi; \
  else \
    echo FAIL > "$@"; \
    { echo "=== stdout diff (expected vs actual) ==="; diff -u "$$exp.n" "$$act.n" || true; \
      echo "=== stderr diff (expected vs actual) ==="; diff -u "$$experr.n" "$$acterr.n" || true; } > "$@.diff" 2>&1; \
    if [ -t 1 ]; then printf F; fi; \
  fi; \
else \
  echo ERR > "$@"; \
  if [ -t 1 ]; then printf E; fi; \
fi; \
rm -f "$$cfile" "$$cfile.o"; \
rm -rf "$$tmpdir"
endef

# Per-package test rules (one pattern rule per bundled package: GNU Make
# patterns allow a single %, so the package name is fixed per rule).
define PKG_TEST_RULE
build/test-results/pkg.$(1).%.ok: packages/$(1)/test/%.rb $$(SP_RT_LIB) $$(SP_RT_MT_LIB) $$(BUNDLED_NATIVE_OBJS) $$(PCH_PLAIN) $$(PCH_NOPOLY) | $$(SPINEL)
	$$(RUN_ONE_TEST)
endef
$(foreach d,$(wildcard packages/*/test),$(eval $(call PKG_TEST_RULE,$(patsubst packages/%/test,%,$(d)))))

build/test-results/%.ok: test/%.rb $(SP_RT_LIB) $(SP_RT_MT_LIB) $(BUNDLED_NATIVE_OBJS) $(PCH_PLAIN) $(PCH_NOPOLY) | $(SPINEL)
	$(RUN_ONE_TEST)

clean-test-results:
	@rm -rf build/test-results

# ---- Expected-output regeneration ----
# Snapshot each test's reference Ruby output so the test target uses the file
# directly and skips per-test ruby. .expected is stdout; .err.expected is stderr
# and is refreshed only where it already exists (a missing one means "stderr
# must be empty" and is left untouched).
EXPECTED_FILES     := $(patsubst test/%.rb,test/%.rb.expected,$(TESTS))
ERR_EXPECTED_FILES := $(wildcard test/*.rb.err.expected)

regen-expected: $(EXPECTED_FILES)
# Separate from regen-expected so a routine stdout refresh never rewrites the
# stderr sidecars (e.g. while a developer is using stderr for debugging).
regen-expected-err: $(ERR_EXPECTED_FILES)

# Benchmark snapshots: `make bench` uses benchmark/<name>.rb.expected when it
# exists and only falls back to running CRuby without one. The oracle runs
# here, once, instead of on every bench invocation -- the CRuby leg was ~2/3
# of bench wall time (bm_range_each alone spends ~25s in CRuby).
BENCH_EXPECTED_FILES := $(patsubst %.rb,%.rb.expected,$(wildcard benchmark/*.rb))
regen-bench-expected: $(BENCH_EXPECTED_FILES)
# Not regen-snapshot: benches need the 60s oracle budget (bm_range_each runs
# ~25s under CRuby), not the 10s test budget.
benchmark/%.rb.expected: benchmark/%.rb
	@rc=0; $(TIMEOUT60) $(REF_RUBY) $< >$@.tmp 2>/dev/null || rc=$$?; \
	if [ $$rc -ne 0 ] && [ "$(REF_RUBY)" != "ruby" ]; then \
	  rc=0; $(TIMEOUT60) ruby $< >$@.tmp 2>/dev/null || rc=$$?; \
	fi; \
	if [ $$rc -ne 0 ]; then \
	  echo "regen-bench-expected: $< failed (rc=$$rc); skipping $@" >&2; rm -f $@.tmp; \
	else \
	  LC_ALL=C sed 's/\r$$//' $@.tmp > $@; rm -f $@.tmp; echo "regen $@"; \
	fi

# Regenerate $@ from the reference Ruby (falling back to a system ruby); $1 is
# the redirection selecting which stream to capture into $@.tmp. A failing
# oracle is skipped so a stale snapshot is kept rather than clobbered.
define regen-snapshot
@args=""; \
if [ -f "$<.args" ]; then args=$$(cat "$<.args"); fi; \
stdinf=/dev/null; \
if [ -f "$<.stdin" ]; then stdinf="$<.stdin"; fi; \
rc=0; $(TIMEOUT10) $(REF_RUBY) $< $$args <"$$stdinf" $1 || rc=$$?; \
if [ $$rc -ne 0 ] && [ "$(REF_RUBY)" != "ruby" ]; then \
  rc=0; $(TIMEOUT10) ruby $< $$args <"$$stdinf" $1 || rc=$$?; \
fi; \
if [ $$rc -ne 0 ]; then \
  echo "regen-expected: $< failed (rc=$$rc); skipping $@" >&2; rm -f $@.tmp; \
else \
  LC_ALL=C sed 's/\r$$//' $@.tmp > $@; rm -f $@.tmp; \
fi
endef

test/%.rb.expected: test/%.rb
	$(call regen-snapshot,>$@.tmp 2>/dev/null)

test/%.rb.err.expected: test/%.rb
	$(call regen-snapshot,2>$@.tmp >/dev/null)

# Each benchmark is independent: compile it, run it, diff against CRuby (or its
# .expected), and drop a one-word verdict file. `xargs -P` runs them across the
# cores (serial under a jobserver -- see BENCH_PJOBS). Scratch paths are keyed by
# the benchmark's basename: unique per benchmark (so parallel workers never
# collide) AND stable across runs, so the generated C's embedded __FILE__ stays
# constant and the cc (ccache) cache keeps hitting -- a per-run mktemp path would
# defeat it. Verdicts are aggregated in benchmark order (deterministic).
bench: $(SPINEL) $(SP_RT_LIB)
	@if [ -z "$(TIMEOUT_BIN)" ]; then echo "Note: no 'timeout' command found; running without time limits."; fi
	@rm -rf build/bench-results; mkdir -p build/bench-results
	@ls benchmark/*.rb | xargs -P $(BENCH_PJOBS) -n 1 sh -c '\
	  f="$$1"; bn=$$(basename "$$f" .rb); d=build/bench-results; res="$$d/$$bn.res"; \
	  c="$$d/$$bn.c"; o="$$d/$$bn.o"; bin="$$d/$$bn.bin"; exp="$$d/$$bn.exp"; act="$$d/$$bn.act"; \
	  if $(TIMEOUT10) $(SPINEL) "$$f" -c --no-line-map -o "$$c" 2>/dev/null \
	     && $(CC) $(CFLAGS) -Werror $(TEST_WARN_SUPPRESS) $(SEC_FLAGS) -Ilib -c "$$c" -o "$$o" 2>/dev/null \
	     && $(CC) $(CFLAGS) "$$o" $(SP_RT_LIB) $(LDFLAGS) -lm $(GC_FLAGS) -o "$$bin" 2>/dev/null; then \
	    if [ -f "$$f.expected" ]; then cp "$$f.expected" "$$exp"; rc=0; \
	    else $(TIMEOUT60) $(REF_RUBY) "$$f" >"$$exp" 2>/dev/null; rc=$$?; \
	      if [ $$rc -ne 0 ] && [ "$(REF_RUBY)" != "ruby" ]; then $(TIMEOUT60) ruby "$$f" >"$$exp" 2>/dev/null; rc=$$?; fi; \
	    fi; \
	    if [ $$rc -eq 124 ]; then echo SKIP >"$$res"; \
	    else $(TIMEOUT60) "$$bin" >"$$act" 2>/dev/null; \
	      LC_ALL=C sed "s/\r$$//" "$$exp" >"$$exp.n"; LC_ALL=C sed "s/\r$$//" "$$act" >"$$act.n"; \
	      if cmp -s "$$exp.n" "$$act.n"; then echo PASS >"$$res"; \
	      else { echo FAIL; diff -u "$$exp.n" "$$act.n" 2>&1 | head -40; } >"$$res"; fi; \
	    fi; \
	  else echo ERR >"$$res"; fi' sh
	@pass=0; fail=0; err=0; skip=0; \
	for r in build/bench-results/*.res; do \
	  [ -e "$$r" ] || continue; \
	  bn=$$(basename "$$r" .res); s=$$(head -1 "$$r"); \
	  case "$$s" in \
	    PASS) pass=$$((pass+1));; \
	    SKIP) echo "SKIP: $$bn (ruby timeout)"; skip=$$((skip+1));; \
	    FAIL) echo "FAIL: $$bn"; tail -n +2 "$$r" | head -40; fail=$$((fail+1));; \
	    *) echo "ERR:  $$bn"; err=$$((err+1));; \
	  esac; \
	done; \
	rm -rf build/bench-results; \
	echo "Benchmarks: $$pass pass, $$fail fail, $$err error, $$skip skip"; \
	if [ $$fail -ne 0 ] || [ $$err -ne 0 ]; then exit 1; fi

# ---- ruby/spec coverage harness (tools/rubyspec/) ----
# Measures CRuby-compatibility coverage: extracts ruby/spec into one program
# per example, classifies each as PASS/FAIL/REJECT/ERROR against spinel, and
# ranks the reject diagnostics -- the "what to implement next" list. Not part
# of the gate (it measures the frontier, it does not defend it).
RUBYSPEC_DIR := build/rubyspec
# Pinned ruby/spec revision: the expectations manifest is only meaningful
# against this exact tree. Bumping it is a deliberate act: re-run the full
# measurement, review the manifest diff, and commit both together.
RUBYSPEC_REV := 79e2dee

$(RUBYSPEC_DIR)/.pinned:
	@if [ ! -d $(RUBYSPEC_DIR) ]; then \
	  git clone https://github.com/ruby/spec $(RUBYSPEC_DIR); \
	fi
	@git -C $(RUBYSPEC_DIR) rev-parse -q --verify $(RUBYSPEC_REV) >/dev/null 2>&1 || \
	  git -C $(RUBYSPEC_DIR) fetch -q origin
	@git -C $(RUBYSPEC_DIR) checkout -q $(RUBYSPEC_REV)
	@touch $@

# Opted-in spec suites: each <dir> maps to expectations/<dir with / -> ->.tsv.
# Add a directory here + generate its manifest (gen_manifest.rb) to enroll it.
# (core/comparable is not enrolled: its specs build fixture classes through
# Module.new/def_method patterns the extractor can't project -- 53 of 54
# extract as HARNESS-SKEW, leaving nothing to defend.)
RUBYSPEC_SUITES := language core/array core/string core/hash core/integer core/range

rubyspec: $(SPINEL) $(RUBYSPEC_DIR)/.pinned
	@for d in $(RUBYSPEC_SUITES); do \
	  nm=$$(echo $$d | tr / -); \
	  echo "=== ruby/spec $$d ==="; \
	  rm -rf build/rubyspec-ex-$$nm && ruby tools/rubyspec/extract.rb $(RUBYSPEC_DIR)/$$d build/rubyspec-ex-$$nm; \
	  bash tools/rubyspec/run.sh build/rubyspec-ex-$$nm build/rubyspec-results-$$nm.tsv; \
	  ruby tools/rubyspec/manifest_diff.rb tools/rubyspec/expectations/$$nm.tsv build/rubyspec-results-$$nm.tsv || true; \
	done

# Retention gate: re-run only the examples the manifests expect to PASS and
# fail on any regression. Improvements (non-PASS -> PASS) never fail this
# target -- they surface in `make rubyspec`'s manifest diff instead, and are
# promoted by regenerating the manifest deliberately.
rubyspec-gate: $(SPINEL) $(RUBYSPEC_DIR)/.pinned
	@ok=1; for d in $(RUBYSPEC_SUITES); do \
	  nm=$$(echo $$d | tr / -); \
	  rm -rf build/rubyspec-ex-$$nm && ruby tools/rubyspec/extract.rb $(RUBYSPEC_DIR)/$$d build/rubyspec-ex-$$nm; \
	  awk -F'\t' '$$2=="PASS"{print $$1}' tools/rubyspec/expectations/$$nm.tsv > build/rubyspec-gate-$$nm.list; \
	  RUBYSPEC_ONLY=build/rubyspec-gate-$$nm.list RUBYSPEC_GATE=1 \
	    bash tools/rubyspec/run.sh build/rubyspec-ex-$$nm build/rubyspec-gate-$$nm.tsv >/dev/null; \
	  bad=$$(awk -F'\t' '$$2!="PASS"' build/rubyspec-gate-$$nm.tsv | wc -l); \
	  if [ $$bad -ne 0 ]; then \
	    echo "rubyspec-gate[$$d]: $$bad regression(s):"; \
	    awk -F'\t' '$$2!="PASS"' build/rubyspec-gate-$$nm.tsv; ok=0; \
	  else \
	    echo "rubyspec-gate[$$d]: all $$(wc -l < build/rubyspec-gate-$$nm.list) expected-PASS examples still pass"; \
	  fi; \
	done; [ $$ok -eq 1 ]

# ---- Optcarrot integration test ----
OPTCARROT_DIR  := build/optcarrot
OPTCARROT_REPO := https://github.com/mame/optcarrot.git
OPTCARROT_BRANCH := experiment/spinel

optcarrot: $(SPINEL) $(SP_RT_LIB)
	@if [ ! -d $(OPTCARROT_DIR) ]; then \
	  git clone --depth=1 --branch=$(OPTCARROT_BRANCH) $(OPTCARROT_REPO) $(OPTCARROT_DIR); \
	fi
	@ruby $(OPTCARROT_DIR)/tools/pack-for-spinel.rb > build/optcarrot-single.rb
	@$(SPINEL) build/optcarrot-single.rb -c --no-line-map -o build/optcarrot-single.c
	@$(CC) $(CFLAGS) -DSP_INT_OVERFLOW_MODE_WRAP -Ilib build/optcarrot-single.c $(SP_RT_LIB) $(LDFLAGS) -lm $(GC_FLAGS) -o build/optcarrot-single
	@n=$${OPTCARROT_RUNS:-5}; fps=""; out=""; \
	for i in $$(seq 1 $$n); do \
	  out=$$($(TIMEOUT60) ./build/optcarrot-single 2>&1); \
	  f=$$(echo "$$out" | sed -n 's/^fps: \([0-9.]*\)$$/\1/p'); \
	  [ -n "$$f" ] && fps="$$fps $$f"; \
	done; \
	echo "$$out" | grep -v '^fps:'; \
	echo "$$fps" | tr ' ' '\n' | grep -v '^$$' | sort -g | \
	  awk -v n="$$n" '{v[NR]=$$1} END { \
	    if (NR==0) exit; \
	    m=(NR%2)?v[(NR+1)/2]:(v[NR/2]+v[NR/2+1])/2; \
	    printf "fps: %.1f  (median of %d; %.1f-%.1f, spread %.1f%%)\n", m, NR, v[1], v[NR], (v[NR]-v[1])/m*100 }'; \
	if echo "$$out" | grep -q "^checksum: 59662$$" && [ -n "$$fps" ]; then \
	  echo "Optcarrot: OK"; \
	else \
	  echo "Optcarrot: FAIL — expected 'fps: <num>' and 'checksum: 59662'"; \
	  exit 1; \
	fi

# ---- Developer gates ----
#
# `test`, `bench` and `optcarrot` only READ the compiler binaries and
# write to disjoint build/ dirs, so they run concurrently as parallel
# prerequisites. Every recursive $(MAKE) is `+`-prefixed so the jobserver
# fd is inherited; none pass an explicit -j (which would force a sub-make
# to spawn its own pool → oversubscription).

# Fast pre-commit: rebuild the compiler and run the suite. OPT=-O1 compiles
# the spinel_rt.h-heavy per-test C ~3x faster than -O0 (the optimizer prunes
# the 800+ unreferenced static fns before codegen). Skips bench/optcarrot —
# run `make gate` before pushing for those.
check:
	+@$(MAKE) --no-print-directory all
	+@$(MAKE) --no-print-directory test OPT=-O1
	+@$(MAKE) --no-print-directory alloc-report-test
	+@$(MAKE) --no-print-directory infer-test
	+@$(MAKE) --no-print-directory spin-check

# SPINEL_ALLOC_REPORT / SPINEL_ALLOC_SITES (#1336): the site is an address, so
# assert the line SHAPE rather than a snapshot -- per-type lines without the
# sites gate, `site;type` lines with it, and the program's own output either way.
# Inference properties the stdout comparison cannot see: a boxed slot still
# prints the right answer, so a type regression here is invisible to `make
# test`. Assert the emitted C signature directly, the way rbs-seed-test does
# for seeds.
infer-test: $(SPINEL) $(SP_RT_LIB)
	@tmp=$$(mktemp -d /tmp/spinel-infer.XXXXXX); ok=1; \
	$(SPINEL) test/infer/unsettled_index_write.rb -c --no-line-map -o "$$tmp/u.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile unsettled_index_write)"; exit 1; }; \
	grep -Eq 'static mrb_int sp_M_s_mul\(mrb_int [A-Za-z_]+, mrb_int [A-Za-z_]+\)' "$$tmp/u.c" || { echo "infer-test: FAIL (an int-keyed []= on an unsettled slot poisoned the call graph)"; grep -E 'sp_M_s_mul\(' "$$tmp/u.c" | head -1; ok=0; }; \
	grep -Eq 'sp_IntArray \* *lv_xs' "$$tmp/u.c" || { echo "infer-test: FAIL (the mapped array did not settle to an int array)"; ok=0; }; \
	$(SPINEL) test/infer/int_keyed_hash.rb -c --no-line-map -o "$$tmp/k.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile int_keyed_hash)"; exit 1; }; \
	grep -Eq 'sp_IntIntHash \* *lv_h' "$$tmp/k.c" || { echo "infer-test: FAIL (a slot with no array evidence lost its int-keyed hash)"; ok=0; }; \
	$(SPINEL) test/infer/int_table_ivar_param.rb -c --no-line-map -o "$$tmp/t.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile int_table_ivar_param)"; exit 1; }; \
	grep -Eq 'static mrb_int sp_F_s_add\(mrb_int [A-Za-z_]+, mrb_int [A-Za-z_]+\)' "$$tmp/t.c" || { echo "infer-test: FAIL (an int table on an ivar poisoned the helper it feeds)"; grep -E 'sp_F_s_add\(' "$$tmp/t.c" | head -1; ok=0; }; \
	grep -Eq 'sp_PtrArray \* *iv_t;' "$$tmp/t.c" || { echo "infer-test: FAIL (the ivar table lost its typed representation)"; ok=0; }; \
	grep -Eq 'sp_IntArray \* *lv_row' "$$tmp/t.c" || { echo "infer-test: FAIL (a row read out of the table stayed boxed)"; ok=0; }; \
	$(SPINEL) test/infer/class_method_table_arg.rb -c --no-line-map -o "$$tmp/m.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile class_method_table_arg)"; exit 1; }; \
	grep -Eq 'sp_PtrArray \* *lv_rows' "$$tmp/m.c" || { echo "infer-test: FAIL (a table passed to a class method lost its typed representation)"; grep -E 'sp_M_s_consume\(' "$$tmp/m.c" | head -1; ok=0; }; \
	grep -Eq 'static mrb_int sp_F_s_mul\(mrb_int [A-Za-z_]+, mrb_int [A-Za-z_]+\)' "$$tmp/m.c" || { echo "infer-test: FAIL (a helper reading an element of the table bound a boxed parameter)"; grep -E 'sp_F_s_mul\(' "$$tmp/m.c" | head -1; ok=0; }; \
	$(SPINEL) test/infer/return_table_across_methods.rb -c --no-line-map -o "$$tmp/r.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile return_table_across_methods)"; exit 1; }; \
	grep -Eq 'static sp_PtrArray \* *sp_T_s_build\(' "$$tmp/r.c" || { echo "infer-test: FAIL (a method returning a table of int arrays stayed a boxed poly array)"; grep -E 'sp_T_s_build\(' "$$tmp/r.c" | head -1; ok=0; }; \
	grep -Eq 'sp_PtrArray \* *lv_rows' "$$tmp/r.c" || { echo "infer-test: FAIL (the caller's table did not follow the callee's return type)"; ok=0; }; \
	grep -Eq 'static mrb_int sp_F_s_mul\(mrb_int [A-Za-z_]+, mrb_int [A-Za-z_]+\)' "$$tmp/r.c" || { echo "infer-test: FAIL (the narrowing was not visible while the helper's parameters bound)"; grep -E 'sp_F_s_mul\(' "$$tmp/r.c" | head -1; ok=0; }; \
	$(SPINEL) test/infer/generator_element_cycle.rb -c --no-line-map -o "$$tmp/g.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile generator_element_cycle)"; exit 1; }; \
	grep -Eq 'static mrb_int sp_F_s_add\(mrb_int [A-Za-z_]+, mrb_int [A-Za-z_]+\)' "$$tmp/g.c" || { echo "infer-test: FAIL (a generator whose element feeds back into its own operands latched a poly array)"; grep -E 'sp_F_s_add\(' "$$tmp/g.c" | head -1; ok=0; }; \
	grep -Eq 'static sp_IntArray \* *sp_E_s_add\(sp_IntArray \*' "$$tmp/g.c" || { echo "infer-test: FAIL (the extension-field add did not settle on the Integer array)"; grep -E 'sp_E_s_add\(' "$$tmp/g.c" | head -1; ok=0; }; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "infer-test: pass"; else exit 1; fi

alloc-report-test: $(SPINEL) $(SP_RT_LIB)
	@tmp=$$(mktemp -d /tmp/spinel-alloc.XXXXXX); ok=1; \
	$(SPINEL) test/alloc-report/sites.rb -o "$$tmp/sites" >/dev/null 2>&1 || { echo "alloc-report-test: FAIL (compile)"; exit 1; }; \
	SPINEL_ALLOC_REPORT="$$tmp/t.folded" "$$tmp/sites" > "$$tmp/t.out" 2>&1; \
	grep -q '^done$$' "$$tmp/t.out" || { echo "alloc-report-test: FAIL (program output)"; ok=0; }; \
	grep -qE '^alloc;[A-Za-z(][^;]* [0-9]+$$' "$$tmp/t.folded" || { echo "alloc-report-test: FAIL (no per-type alloc line)"; sed -n 1,5p "$$tmp/t.folded"; ok=0; }; \
	grep -qE '^# bytes ' "$$tmp/t.folded" || { echo "alloc-report-test: FAIL (no bytes line)"; ok=0; }; \
	SPINEL_ALLOC_REPORT="$$tmp/s.folded" SPINEL_ALLOC_SITES=1 "$$tmp/sites" > "$$tmp/s.out" 2>&1; \
	grep -q '^done$$' "$$tmp/s.out" || { echo "alloc-report-test: FAIL (program output with sites)"; ok=0; }; \
	if grep -qE '^alloc;.+;[A-Za-z(][^;]* [0-9]+$$' "$$tmp/s.folded"; then :; \
	else grep -qE '^alloc;[A-Za-z(][^;]* [0-9]+$$' "$$tmp/s.folded" || { echo "alloc-report-test: FAIL (sites run produced neither shape)"; sed -n 1,5p "$$tmp/s.folded"; ok=0; }; fi; \
	grep -qE '^alloc;[^;]*String [0-9]+$$' "$$tmp/t.folded" || { echo "alloc-report-test: FAIL (no String line without sites)"; ok=0; }; \
	grep -qE '^# bytes .*String [0-9]+$$' "$$tmp/t.folded" || { echo "alloc-report-test: FAIL (no String bytes line)"; ok=0; }; \
	if grep -qE '^alloc;.+;[A-Za-z(][^;]* [0-9]+$$' "$$tmp/s.folded"; then \
	  grep -qE '^alloc;.+;String [0-9]+$$' "$$tmp/s.folded" || { echo "alloc-report-test: FAIL (String has no site while other types do)"; grep String "$$tmp/s.folded" | head -2; ok=0; }; \
	fi; \
	$(SPINEL) test/alloc-report/straight_append.rb -o "$$tmp/sa" >/dev/null 2>&1 || { echo "alloc-report-test: FAIL (compile straight_append)"; exit 1; }; \
	SPINEL_ALLOC_REPORT="$$tmp/sa.folded" "$$tmp/sa" > "$$tmp/sa.out" 2>&1; \
	grep -q '^960$$' "$$tmp/sa.out" || { echo "alloc-report-test: FAIL (straight_append wrong length)"; ok=0; }; \
	awk '/^# bytes .*String /{n=$$NF} END{ if (n == "" || n+0 > 5000) exit 1 }' "$$tmp/sa.folded" || { echo "alloc-report-test: FAIL (straight-line appends allocate a multiple of the result: quadratic is back)"; grep String "$$tmp/sa.folded"; ok=0; }; \
	grep -q '^alloc;(unattributed) ' "$$tmp/s.folded" && { echo "alloc-report-test: FAIL (the stats table saturated on a normal run)"; ok=0; }; \
	$(CC) $(CFLAGS) -DSP_ALLOC_STATS=2 -Ilib -c lib/sp_alloc.c -o "$$tmp/sm.o" 2>/dev/null || { echo "alloc-report-test: FAIL (compile small-table sp_alloc)"; exit 1; }; \
	cp $(SP_RT_LIB) "$$tmp/sm.a" && ar d "$$tmp/sm.a" sp_alloc.o 2>/dev/null && ar r "$$tmp/sm.a" "$$tmp/sm.o" 2>/dev/null; \
	$(SPINEL) test/alloc-report/sites.rb -c --no-line-map -o "$$tmp/sm.c" >/dev/null 2>&1; \
	$(CC) $(CFLAGS) -Ilib "$$tmp/sm.c" "$$tmp/sm.a" $(LDFLAGS) -lm $(GC_FLAGS) -o "$$tmp/sm" 2>/dev/null || { echo "alloc-report-test: FAIL (link small-table binary)"; exit 1; }; \
	SPINEL_ALLOC_REPORT="$$tmp/sm.folded" SPINEL_ALLOC_SITES=1 "$$tmp/sm" >/dev/null 2>&1; \
	grep -q '^alloc;(unattributed) ' "$$tmp/sm.folded" || { echo "alloc-report-test: FAIL (a saturated table said nothing about it)"; sed -n 1,8p "$$tmp/sm.folded"; ok=0; }; \
	grep -q '^# note the stats table' "$$tmp/sm.folded" || { echo "alloc-report-test: FAIL (no note explaining the saturated run)"; ok=0; }; \
	full=$$(awk '/;\(no-scan\) /{print $$NF}' "$$tmp/s.folded" | head -1); \
	sat=$$(awk '/;\(no-scan\) /{print $$NF}' "$$tmp/sm.folded" | head -1); \
	[ -n "$$full" ] && [ "$$full" = "$$sat" ] || { echo "alloc-report-test: FAIL (a saturated run changed a surviving row: $$full vs $$sat)"; ok=0; }; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "alloc-report-test: pass"; else exit 1; fi

# spin end-to-end: scaffold/path-dep/git-dep/lock/vendor/offline/test,
# hermetic under a mktemp dir (tools/spin_e2e.sh).
spin-check: bin/spin
	@tools/spin_e2e.sh bin/spin

# Full pre-push gate: test || bench || optcarrot in parallel.
gate:
	+@$(MAKE) --no-print-directory all
	+@$(MAKE) --no-print-directory gate-legs
	@echo "gate: ALL GREEN"

gate-legs: gate-test gate-bench gate-optcarrot gate-rubyspec
gate-test:
	+@$(MAKE) --no-print-directory test OPT=-O1
gate-bench:
	+@$(MAKE) --no-print-directory bench
gate-optcarrot:
	+@$(MAKE) --no-print-directory optcarrot
gate-rubyspec:
	+@$(MAKE) --no-print-directory rubyspec-gate

# ---- Install ----

PREFIX   ?= /usr/local
SPNLDIR   = $(PREFIX)/lib/spinel

# Install the compiler, the spin project tool, and the runtime.
install: all bin/spin
	install -d $(SPNLDIR)/lib
	install -m 755 $(SPINEL)            $(SPNLDIR)/spinel
	install -m 755 bin/spin             $(SPNLDIR)/spin
	@# spinel_rbs_extract is a sibling of spinel at runtime (main.c looks for it
	@# there for --rbs). spin now passes --rbs for .rbs-carrying packages, so
	@# omitting it here silently drops seeds on installed toolchains (#1792).
	@if [ -x "$(RBS_EXTRACT_BIN)" ]; then \
	  install -m 755 $(RBS_EXTRACT_BIN) $(SPNLDIR)/spinel_rbs_extract; \
	else \
	  echo "note: spinel_rbs_extract not built (RBS parser absent via 'make deps'); --rbs seeds will be unavailable in this install"; \
	fi
	install -m 644 lib/libspinel_rt.a    $(SPNLDIR)/lib/
	install -m 644 lib/libspinel_rt_mt.a $(SPNLDIR)/lib/
	install -m 644 lib/spinel_rt.h      $(SPNLDIR)/lib/
	install -m 644 lib/sp_types.h        $(SPNLDIR)/lib/
	install -m 644 lib/sp_core.h         $(SPNLDIR)/lib/
	install -m 644 lib/sp_system.h       $(SPNLDIR)/lib/
	install -m 644 lib/sp_gc.h           $(SPNLDIR)/lib/
	install -m 644 lib/sp_alloc.h        $(SPNLDIR)/lib/
	install -d $(SPNLDIR)/lib/spinel
	install -m 644 lib/spinel/runtime.h  $(SPNLDIR)/lib/spinel/
	install -m 644 lib/sp_marshal.h      $(SPNLDIR)/lib/
	install -m 644 lib/sp_format.h       $(SPNLDIR)/lib/
	install -m 644 lib/sp_string.h       $(SPNLDIR)/lib/
	install -m 644 lib/sp_inspect.h      $(SPNLDIR)/lib/
	install -m 644 lib/sp_array.h        $(SPNLDIR)/lib/
	install -m 644 lib/sp_str.h          $(SPNLDIR)/lib/
	install -m 644 lib/sp_hash.h         $(SPNLDIR)/lib/
	install -m 644 lib/sp_proc.h         $(SPNLDIR)/lib/
	install -m 644 lib/sp_exc.h          $(SPNLDIR)/lib/
	install -m 644 lib/sp_range.h        $(SPNLDIR)/lib/
	install -m 644 lib/sp_argf.h         $(SPNLDIR)/lib/
	install -m 644 lib/sp_re.h           $(SPNLDIR)/lib/
	install -m 644 lib/sp_fiber.h        $(SPNLDIR)/lib/
	install -m 644 lib/sp_fiber_ctx.h    $(SPNLDIR)/lib/
	install -m 644 lib/sp_sched.h        $(SPNLDIR)/lib/
	install -m 644 lib/sp_io.h           $(SPNLDIR)/lib/
	install -m 644 lib/sp_time.h         $(SPNLDIR)/lib/
	install -m 644 lib/sp_net.h          $(SPNLDIR)/lib/
	install -m 644 lib/sp_random.h       $(SPNLDIR)/lib/
	install -m 644 lib/sp_enum.h         $(SPNLDIR)/lib/
	rm -rf $(SPNLDIR)/packages
	cp -r packages $(SPNLDIR)/packages
	rm -rf $(SPNLDIR)/packages/*/build
	install -d $(PREFIX)/bin
	ln -sf $(SPNLDIR)/spinel $(PREFIX)/bin/spinel
	ln -sf $(SPNLDIR)/spin   $(PREFIX)/bin/spin
	for t in $(TOOL_NAMES); do \
	  install -m 755 bin/spinel-$$t $(PREFIX)/bin/spinel-$$t; \
	done

uninstall:
	rm -f $(PREFIX)/bin/spinel $(PREFIX)/bin/spin
	for t in $(TOOL_NAMES); do rm -f $(PREFIX)/bin/spinel-$$t; done
	rm -rf $(SPNLDIR)

# ---- Clean ----

clean:
	rm -rf build/ bin/
	rm -f spinel
