# Spinel AOT Compiler - Makefile
#
# Usage:
#   make              Build the C compiler (runtime + spinel + tools)
#   make test         Run the feature tests (always a fresh run)
#   make bench        Run benchmarks vs CRuby
#   make bench-compile  Time analysis/emission on a synthetic program at K=100, 200
#   make optcarrot    End-to-end optcarrot integration test
#   make check        Fast pre-commit: rebuild + tests
#   make gate         Full pre-push: test || bench || optcarrot (reuses the
#                     passes of unchanged programs, see RUN_ONE_TEST)
#   make gate-full    The same with GATE_CACHE=0: every program built and run
#   make clean        Remove built binaries

# COPT: optimization level override. Default -O2 for release builds.
# Set `COPT=-O0 -g0` in config.mk for fast iteration / debugging.
# A command-line `make COPT=-O0` takes precedence.
COPT ?= -O2
# PORTABLE=1 compiles the runtime and the test programs with -DSP_PORTABLE:
# every fallback in lib/sp_compat.h, even where the GNU extension exists, so a
# GCC or clang tree runs the code a plain C compiler would get. Use a tree of
# its own (the objects differ).
ifeq ($(PORTABLE),1)
COPT += -DSP_PORTABLE
endif
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

.PHONY: all regexp wasm-rt wasm-test rbs_extract rbs-test rbs-seed-test rbs-seed-extractor cident plan-check-test repr-check-test traits-check-test bop-arity-check-test re-lit-test reject-test cli-opts-test defer-refusals-test check-stores-test backtrace-test gc-minor-test thread-puts-test ext-test ext-cruby-test alloc-report-test rubyspec rubyspec-gate spin-check \
        test test-run clean-test-results regen-rbs-expected \
        regen-expected regen-expected-err bench optcarrot gate gate-full check gate-legs gate-test gate-bench gc-phases-test gc-str-major-test threaded-render-test gc-locality-test test-corpus test-corpus-summary \
        gate-optcarrot scale-test clean install uninstall deps tools

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
# The probe compiles AND LINKS a use of the API sp_openssl.c needs, not just
# the header: a system with headers but no libssl, or one too old for
# TLS_client_method, would otherwise pass the header check and fail at the
# package's own link.
#
# KEG-ONLY INSTALLS. On a Homebrew host the headers and libraries are real
# but off the default search path, so a bare $(CC) fails this probe and the
# whole package -- sp_openssl.o, `require "openssl"`, and every
# packages/openssl/test/*.rb -- drops out of the build without saying so.
# A green `make test` there has not exercised the package at all, which is
# how an .expected file for it can be written from a run that never
# happened.
#
# So: probe bare first, and only if that fails ask for a prefix and probe
# AGAIN with -I/-L. A prefix is trusted only when it passes the same
# compile-and-link probe, so a stale or partial install cannot flip this to
# yes and then fail at the package's own link. On a host where the bare
# probe already passes, nothing below runs and the build is unchanged.
# TWO questions, because they have different answers. The link probe says
# libssl is here and usable. The syntax-only compile of the package's own
# source says THIS file can be built against it -- a version floor, a renamed
# API, a missing macro. A header set that answers the first and not the second
# (LibreSSL 3.1.5, whose TLS_client_method exists but whose evp.h has no
# EVP_CTRL_AEAD_SET_IVLEN) used to pass the probe and then stop `make` in the
# middle with a #error. Compiling the real file rather than a copy of its
# version guard is what keeps the two from drifting (#4253).
SP_OSSL_PROBE = $(shell printf '\043include <openssl/ssl.h>\nint main(void){return TLS_client_method()!=0;}\n' > /tmp/sp_ossl_probe.c 2>/dev/null && $(CC) $(1) /tmp/sp_ossl_probe.c -lssl -lcrypto -o /tmp/sp_ossl_probe >/dev/null 2>&1 && $(CC) $(1) -fsyntax-only -Ilib -Ipackages/openssl packages/openssl/sp_openssl.c >/dev/null 2>&1 && echo yes)
OPENSSL_AVAILABLE := $(call SP_OSSL_PROBE,)
ifneq ($(OPENSSL_AVAILABLE),yes)
# `brew --prefix` first: it knows a non-default HOMEBREW_PREFIX, which the
# hardcoded pair below does not. The pair is the fallback for a host with
# the install but without brew on PATH (a CI image that untars it, say).
OPENSSL_PREFIX := $(firstword $(wildcard \
    $(shell brew --prefix openssl@3 2>/dev/null) \
    $(shell brew --prefix openssl 2>/dev/null) \
    /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3))
ifneq ($(OPENSSL_PREFIX),)
OPENSSL_CPPFLAGS := -I$(OPENSSL_PREFIX)/include
OPENSSL_LDFLAGS  := -L$(OPENSSL_PREFIX)/lib
OPENSSL_AVAILABLE := $(call SP_OSSL_PROBE,$(OPENSSL_CPPFLAGS) $(OPENSSL_LDFLAGS))
ifeq ($(OPENSSL_AVAILABLE),yes)
# The compiler shells out to cc for a program's final link, and the -lssl
# that openssl.rb's ffi_lib puts on that line needs the -L too. Exported
# rather than threaded through each recipe because the link happens inside
# `bin/spinel`, one process further down. Set ONLY on a host that needed a
# prefix, so a default host's environment is untouched.
export CPATH := $(OPENSSL_PREFIX)/include$(if $(CPATH),:$(CPATH))
export LIBRARY_PATH := $(OPENSSL_PREFIX)/lib$(if $(LIBRARY_PATH),:$(LIBRARY_PATH))
# LIBRARY_PATH is the LINKER's search path; it says nothing to the loader.
# On macOS that is enough -- a Homebrew dylib's install name is absolute
# (`otool -D` on libssl.dylib prints the keg path), so a linked binary
# already knows where to find it at run time. An ELF host is the other way
# round: the -L leaves no RUNPATH behind, so a package test would link and
# then die at exec with `libssl.so.3: cannot open shared object file`. Same
# reason as the two above for being an export rather than a flag -- the run
# is a child process of the test recipe.
export LD_LIBRARY_PATH := $(OPENSSL_PREFIX)/lib$(if $(LD_LIBRARY_PATH),:$(LD_LIBRARY_PATH))
endif
endif
endif
BUNDLED_NATIVE_OBJS = packages/json/sp_json.o packages/stringio/sp_stringio.o packages/strscan/sp_strscan.o packages/base64/sp_base64.o packages/tmpdir/sp_tmpdir.o packages/zlib/sp_zlib.o
ifeq ($(OPENSSL_AVAILABLE),yes)
BUNDLED_NATIVE_OBJS += packages/openssl/sp_openssl.o
endif
# ffi is glue over the SYSTEM libffi (and dlopen), so like openssl it is built
# only where the headers are: without its object `require "ffi"` is the
# builtin FFI DSL's, as before the package. -lffi reaches the link line
# through ffi.rb's ffi_lib. The probe compiles the real file.
LIBFFI_PREFIX := $(firstword $(wildcard $(shell brew --prefix libffi 2>/dev/null)))
LIBFFI_CPPFLAGS := $(shell pkg-config --cflags libffi 2>/dev/null)
LIBFFI_LIBDIR := $(patsubst -L%,%,$(firstword $(filter -L%,$(shell pkg-config --libs-only-L libffi 2>/dev/null))))
# No pkg-config entry: a Homebrew keg is still a known place to look.
ifeq ($(strip $(LIBFFI_CPPFLAGS)$(LIBFFI_LIBDIR)),)
ifneq ($(LIBFFI_PREFIX),)
LIBFFI_CPPFLAGS := -I$(LIBFFI_PREFIX)/include
LIBFFI_LIBDIR := $(LIBFFI_PREFIX)/lib
endif
endif
FFI_AVAILABLE := $(shell printf 'int main(void){return 0;}\n' > /tmp/sp_ffi_probe.c 2>/dev/null && $(CC) /tmp/sp_ffi_probe.c $(if $(LIBFFI_LIBDIR),-L$(LIBFFI_LIBDIR)) -lffi -o /tmp/sp_ffi_probe >/dev/null 2>&1 && $(CC) $(LIBFFI_CPPFLAGS) -fsyntax-only -Ilib -Ipackages/ffi packages/ffi/sp_ffi.c >/dev/null 2>&1 && echo yes)
# A libffi outside the default search path: the -lffi that ffi.rb's ffi_lib
# puts on a program's link line, and the loader at run time, need the
# directory too -- exported for the same reasons as OPENSSL_PREFIX's above.
ifneq ($(LIBFFI_LIBDIR),)
export LIBRARY_PATH := $(LIBFFI_LIBDIR)$(if $(LIBRARY_PATH),:$(LIBRARY_PATH))
export LD_LIBRARY_PATH := $(LIBFFI_LIBDIR)$(if $(LD_LIBRARY_PATH),:$(LD_LIBRARY_PATH))
endif
# The package lays out pointers and `long` as 64-bit: not on a 32-bit target.
ifeq ($(SPINEL_INT_BITS),32)
FFI_AVAILABLE := no
endif
ifeq ($(FFI_AVAILABLE),yes)
BUNDLED_NATIVE_OBJS += packages/ffi/sp_ffi.o
endif
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
# sources (the .gem ships the generated headers -- no rake/bundler needed).
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

# A source archive that builds with no network: the tree at HEAD as git sees
# it, plus the two vendored parsers `make deps` would otherwise fetch from
# rubygems.org (#4447). Named after the release the tree belongs to (the same
# string `spinel --version` prints), so `make dist` at a release tag is the
# release's own tarball; .github/workflows/release.yml attaches it to the
# GitHub release when a tag is pushed. The recipient runs `make` -- `deps` is
# already satisfied by the vendored sources -- and needs only a C compiler.
DIST_RELEASE = $(shell git describe --tags --match '[0-9][0-9][0-9][0-9].[0-9][0-9].[0-9][0-9]' \
                 --match '[0-9][0-9][0-9][0-9].[0-9][0-9].[0-9][0-9].[0-9]*' 2>/dev/null \
                 | sed -e 's/^$$/unreleased/' -e 's/-\([0-9][0-9]*\)-g[0-9a-f]*$$/+\1/')
DIST_NAME = spinel-$(if $(DIST_RELEASE),$(DIST_RELEASE),unreleased)
.PHONY: dist
dist: deps
	@mkdir -p build/dist
	@rm -rf build/dist/$(DIST_NAME) build/dist/$(DIST_NAME).tar.xz
	git archive --format=tar --prefix=$(DIST_NAME)/ HEAD | tar -xf - -C build/dist
	@printf '%s\n%s\n' "$$(git rev-parse --short HEAD)" "$(DIST_RELEASE)" > build/dist/$(DIST_NAME)/.spinel-dist
	@mkdir -p build/dist/$(DIST_NAME)/vendor
	cp -R vendor/prism vendor/rbs build/dist/$(DIST_NAME)/vendor/
	tar -C build/dist -cJf build/dist/$(DIST_NAME).tar.xz $(DIST_NAME)
	@rm -rf build/dist/$(DIST_NAME)
	@ls -l build/dist/$(DIST_NAME).tar.xz

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

SPINEL_HDRS = src/builtin_ops.h src/codegen_call_arms.h src/builtin_names.h src/ty_traits.inc src/call_plan.h src/codegen_poly.h src/repr.h src/node_table.h src/codegen.h src/codegen_internal.h src/types.h src/compiler.h src/analyze.h src/analyze_internal.h src/ffi_spec.h src/csplit.h
build/csrc/analyze_desugar.o build/csrc-work/analyze_desugar.o build/csrc/codegen_call.o build/csrc-work/codegen_call.o: $(wildcard src/*_method_names.inc)
SPINEL_OBJ  = build/csrc/node_table.o build/csrc/types.o build/csrc/compiler.o \
               build/csrc/ffi_spec.o \
               build/csrc/analyze.o build/csrc/analyze_util.o build/csrc/analyze_infer.o build/csrc/analyze_infer_recv.o \
               build/csrc/analyze_scope.o build/csrc/analyze_pass.o build/csrc/analyze_desugar.o build/csrc/repr.o build/csrc/codegen.o build/csrc/codegen_util.o build/csrc/ty_traits_check.o \
               build/csrc/codegen_fold.o build/csrc/codegen_call.o build/csrc/codegen_call_poly.o build/csrc/codegen_call_method.o build/csrc/codegen_call_io.o build/csrc/codegen_call_kernel.o build/csrc/codegen_call_exception.o build/csrc/codegen_call_module.o build/csrc/codegen_call_string.o build/csrc/codegen_call_class.o build/csrc/codegen_call_operator.o build/csrc/codegen_call_object.o build/csrc/codegen_ops.o build/csrc/codegen_call_concurrency.o build/csrc/codegen_call_numeric.o build/csrc/codegen_call_hash.o build/csrc/codegen_call_array.o build/csrc/codegen_view.o build/csrc/builtin_ops.o build/csrc/builtin_names.o build/csrc/codegen_call_recv.o build/csrc/codegen_iter.o build/csrc/call_plan.o build/csrc/codegen_poly_plan.o \
               build/csrc/codegen_expr.o build/csrc/codegen_stmt.o build/csrc/csplit.o build/csrc/main.o

build/csrc:
	@mkdir -p build/csrc

# -Werror=return-type: a compiler function that falls off its end returns
# garbage under -O2, and the CFLAGS this is built with may turn the warning
# off (-Wno-all); a moved rule once lost its last return this way.
build/csrc/%.o: src/%.c $(SPINEL_HDRS) | build/csrc
	$(CC) $(CFLAGS) -Werror=return-type -Isrc -Ibuild/csrc -c $< -o $@

# Build revision, embedded in `spinel --version` (and spin's probe records).
# cmp-guarded so only a HEAD move recompiles main.o, not every build.
# The tmp name carries the PID: gate runs test/bench/optcarrot as parallel
# sub-makes, each re-evaluating this FORCE target -- a shared tmp name races
# (one job's rm strands the other's mv mid-flight).
# SPINEL_RELEASE is the release this build belongs to, read from the nearest
# tag: "2026.09.08" when HEAD is the release, "2026.09.08+12" when it is twelve
# commits past it, "unreleased" before the first tag. The --match patterns ARE
# the format rule -- a tag shaped any other way is not a release, and is
# ignored here rather than breaking anyone's build. The revision is git's own
# short form (unique in the repository, seven digits at least): it is what
# identifies a build (two builds of one release share a name and differ
# here), and tools/spin.rb reads it from the parentheses of `spinel --version`
# for the toolchain key its probe records are stored under.
# A source archive from `make dist` carries no .git: it records the revision
# and release it was cut from in .spinel-dist, and a build from it reads them
# there so its `spinel --version` names the build it is.
build/csrc/spinel_rev.h: FORCE | build/csrc
	@r=$$(git rev-parse --short HEAD 2>/dev/null); \
	d=$$(git describe --tags --match '[0-9][0-9][0-9][0-9].[0-9][0-9].[0-9][0-9]' \
	       --match '[0-9][0-9][0-9][0-9].[0-9][0-9].[0-9][0-9].[0-9]*' 2>/dev/null); \
	if [ -z "$$r" ] && [ -f .spinel-dist ]; then r=$$(sed -n 1p .spinel-dist); d=$$(sed -n 2p .spinel-dist); fi; \
	[ -n "$$r" ] || r=unknown; \
	case "$$d" in \
	  "") d=unreleased ;; \
	  *-*-g*) d="$${d%-*-g*}+$$(echo "$$d" | sed 's/.*-\([0-9][0-9]*\)-g[0-9a-f]*$$/\1/')" ;; \
	esac; \
	t=$@.tmp.$$$$; \
	{ echo "#define SPINEL_BUILD_REV \"$$r\""; \
	  echo "#define SPINEL_RELEASE \"$$d\""; } > $$t; \
	if cmp -s $$t $@; then rm -f $$t; else mv $$t $@; fi

build/csrc/main.o: build/csrc/spinel_rev.h

# The names a top-level Ruby method must not take: every first segment of an
# sp_* identifier the runtime owns. Hand-keeping this list is what let `def gcd`
# and `def gets` collide with sp_gcd / sp_gets -- the set is a fact about the
# runtime sources, so read it from them. Deliberately over-inclusive: an extra
# name only means one more method carries the rb_ infix, while a missing one is
# a C redeclaration in generated code. `rb` itself is in so a user `def rb_x`
# cannot land on the same symbol as an infixed `def x`. cmp-guarded like
# spinel_rev.h, so only a real change recompiles.
SP_RT_NAME_SRC = $(wildcard lib/*.h lib/*.c packages/*/*.h packages/*/*.c)

build/csrc/sp_rt_names.h: $(SP_RT_NAME_SRC) | build/csrc
	@t=$@.tmp.$$$$; \
	{ echo "/* generated from the runtime sources; see the Makefile rule */"; \
	  echo "static const char *const SP_RT_PREFIXES[] = {"; \
	  { grep -hoE '\bsp_[a-z][a-z0-9_]*' $(SP_RT_NAME_SRC) 2>/dev/null \
	    | sed 's/^sp_//' | cut -d_ -f1; echo rb; } \
	    | sort -u | sed 's/.*/  "&",/'; \
	  echo "  NULL"; echo "};"; } > $$t; \
	if cmp -s $$t $@; then rm -f $$t; else mv $$t $@; fi

build/csrc/codegen_util.o: build/csrc/sp_rt_names.h

FORCE:

build/csrc/sp_parse_lib.o: src/spinel_parse.c src/sp_macro.c $(PRISM_LIB) | build/csrc
	$(CC) $(CFLAGS) -I$(PRISM_INC) -c src/spinel_parse.c -o $@

# The compiler links the regexp engine so it can compile a literal at build
# time and refuse one the engine cannot read, where it used to reach the
# engine only at the built program's startup. src/re_lit_check.c is the seam
# (and carries the sp_sprintf the engine's object file references).
# the regexp engine is mruby-regexp as mruby carries it; shim/ answers its
# mruby API and re_spinel.c is spinel's side of it
RE_HDRS = $(wildcard lib/regexp/*.h lib/regexp/*.inc lib/regexp/shim/*.h lib/regexp/shim/mruby/*.h)
build/csrc/re_lit_check.o: src/re_lit_check.c $(RE_HDRS) | build/csrc
	$(CC) $(CFLAGS) -Ilib/regexp -Ilib/regexp/shim -c src/re_lit_check.c -o $@

# Defined HERE, above the first rule that names it. GNU make expands a rule's
# prerequisites when the rule is READ, so with this further down the file the
# $(RE_OBJ) below expanded to nothing: `make bin/spinel` linked whatever
# regexp objects happened to be on disk and never rebuilt a stale one. The
# recipe's own $(RE_OBJ) expands at run time, so the link named the right
# files -- which is what made it look like the compiler was ignoring an edit.
RE_SRC = lib/regexp/re_compile.c lib/regexp/re_exec.c lib/regexp/re_utf8.c lib/regexp/unicase.c lib/regexp/re_spinel.c
RE_OBJ = $(patsubst lib/regexp/%.c,build/regexp/%.o,$(RE_SRC))

$(SPINEL): $(SPINEL_OBJ) build/csrc/sp_parse_lib.o build/csrc/re_lit_check.o $(RE_OBJ) $(PRISM_LIB)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SPINEL_OBJ) build/csrc/sp_parse_lib.o build/csrc/re_lit_check.o $(RE_OBJ) $(PRISM_LIB) -lm $(LDFLAGS) -o $@
	@# Dev convenience: a repo-root `./spinel` pointing at the built binary
	@# (the installed command is `spinel` too). Best-effort; gitignored.
	@ln -sf $@ spinel 2>/dev/null || cp $@ spinel 2>/dev/null || true

# The compiler again with SP_WORK_COUNT: every node access counts one unit and
# the total is printed at exit (see NT_WORK in src/node_table.h). Only the
# scaling test uses it; -O1 because the count does not depend on optimization.
SPINEL_WORK = build/spinel-work
SPINEL_WORK_OBJ = $(patsubst build/csrc/%.o,build/csrc-work/%.o,$(SPINEL_OBJ))
build/csrc-work/%.o: src/%.c $(SPINEL_HDRS) | build/csrc
	@mkdir -p build/csrc-work
	$(CC) $(CFLAGS) -O1 -DSP_WORK_COUNT -Isrc -Ibuild/csrc -c $< -o $@
# the generated headers, as for the plain objects above: without the second,
# `make build/spinel-work` (or `make scale-test`) in a fresh tree compiled
# codegen_util.c before sp_rt_names.h existed and stopped
build/csrc-work/main.o: build/csrc/spinel_rev.h
build/csrc-work/codegen_util.o: build/csrc/sp_rt_names.h
$(SPINEL_WORK): $(SPINEL_WORK_OBJ) build/csrc/sp_parse_lib.o build/csrc/re_lit_check.o $(RE_OBJ) $(PRISM_LIB)
	$(CC) $(CFLAGS) $(SPINEL_WORK_OBJ) build/csrc/sp_parse_lib.o build/csrc/re_lit_check.o $(RE_OBJ) $(PRISM_LIB) -lm $(LDFLAGS) -o $@

# Wrapper around the system `timeout` that always returns GNU coreutils'
# exit code (124 on timeout), regardless of which `timeout` is on PATH.
# The bench target keys on 124 to mark a run as SKIP; busybox uses 143
# and BSDs use 399, which would be misclassified. Built once from C.
$(SPINEL_TIMEOUT): scripts/spinel-timeout.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $< -o $@

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

# RE_CASE_FLAGS: the Unicode tables the regexp engine carries. Either of
# -DRE_NO_UNICODE_CASE and -DRE_NO_UNICODE_CTYPE is mruby's MRB_USE_ASCII_CTYPE
# build (lib/regexp/shim/mruby.h): ASCII-only /i folding, POSIX brackets and
# word boundaries, without the case, type and property tables (a category or
# an emoji property is then refused).
RE_CASE_FLAGS ?=

build/regexp/%.o: lib/regexp/%.c $(RE_HDRS)
	@mkdir -p build/regexp
	$(CC) -c $(COPT) $(SEC_FLAGS) $(RE_CASE_FLAGS) -Ilib/regexp -Ilib/regexp/shim $< -o $@

RT_HDRS = $(wildcard lib/*.h lib/regexp/*.h lib/regexp/*.inc lib/regexp/shim/*.h lib/regexp/shim/mruby/*.h)

# One rule for every lib/*.c object. The per-object header lists here used to be
# written by hand and had drifted: build/sp_array.o never named lib/sp_str.h,
# though sp_array.h includes it, so an edit to sp_str.h left a stale sp_array.o
# in the archive while the generated TU -- which includes the header directly --
# picked the change up. The two halves of one program then disagreed about an
# inline function and nothing said so. The threaded variant below already
# depended on the whole header set; this is the same answer for this half.
build/%.o: lib/%.c $(RT_HDRS)
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib $< -o $@


# Bundled carried-C spin packages (Path B): compiled standalone, NOT into the
# runtime archive, and linked on demand only when the program requires them
# (native_obj markers -> src/main.c). Package C compiles against the stable
# spinel/runtime.h ABI (-Ilib) plus its own package headers. BUNDLED_NATIVE_OBJS
# is defined near the top (before `all`, whose prereqs expand at parse time).
packages/json/sp_json.o: packages/json/sp_json.c packages/json/sp_json.h \
                         lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_compat.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib -Ipackages/json packages/json/sp_json.c -o $@
packages/json/sp_json_mt.o: packages/json/sp_json.c packages/json/sp_json.h \
                            lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_compat.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(PKG_MT_FLAGS) -Ilib -Ipackages/json packages/json/sp_json.c -o $@

# openssl is glue over the SYSTEM libssl, so unlike the other package C it is
# built only when the headers are installed: OPENSSL_AVAILABLE probes for
# <openssl/ssl.h> and the object drops out of BUNDLED_NATIVE_OBJS when it is
# missing, leaving `require "openssl"` an unsatisfiable require rather than a
# link error. -lssl/-lcrypto reach the link line through openssl.rb's ffi_lib,
# which is parsed only when a program requires it.
packages/openssl/sp_openssl.o: packages/openssl/sp_openssl.c \
                               lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_compat.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(OPENSSL_CPPFLAGS) -Ilib -Ipackages/openssl packages/openssl/sp_openssl.c -o $@
packages/openssl/sp_openssl_mt.o: packages/openssl/sp_openssl.c \
                                  lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_compat.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(PKG_MT_FLAGS) $(OPENSSL_CPPFLAGS) -Ilib -Ipackages/openssl packages/openssl/sp_openssl.c -o $@

packages/ffi/sp_ffi.o: packages/ffi/sp_ffi.c \
                       lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_compat.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(LIBFFI_CPPFLAGS) -Ilib -Ipackages/ffi packages/ffi/sp_ffi.c -o $@
packages/ffi/sp_ffi_mt.o: packages/ffi/sp_ffi.c \
                          lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_compat.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(PKG_MT_FLAGS) $(LIBFFI_CPPFLAGS) -Ilib -Ipackages/ffi packages/ffi/sp_ffi.c -o $@

# stringio is a native-bound spin package (Path B typed object): the struct,
# every method, and the header live in the package; the compiler knows it only
# through the native_* declarations in stringio.rb.
packages/stringio/sp_stringio.o: packages/stringio/sp_stringio.c packages/stringio/sp_stringio.h \
                                 lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_compat.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib -Ipackages/stringio packages/stringio/sp_stringio.c -o $@
packages/stringio/sp_stringio_mt.o: packages/stringio/sp_stringio.c packages/stringio/sp_stringio.h \
                                    lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_compat.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(PKG_MT_FLAGS) -Ilib -Ipackages/stringio packages/stringio/sp_stringio.c -o $@

# strscan is likewise a native-bound spin package; its regex matching links
# against the runtime archive's re_exec (a forward extern in the package C).
packages/strscan/sp_strscan.o: packages/strscan/sp_strscan.c \
                               lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_compat.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib packages/strscan/sp_strscan.c -o $@
packages/strscan/sp_strscan_mt.o: packages/strscan/sp_strscan.c \
                                  lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_compat.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(PKG_MT_FLAGS) -Ilib packages/strscan/sp_strscan.c -o $@

# base64 carries its whole implementation; digest carries none (it binds the
# runtime's vendored sp_crypto symbols and has no object of its own).
packages/base64/sp_base64.o: packages/base64/sp_base64.c \
                             lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_compat.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib packages/base64/sp_base64.c -o $@
packages/base64/sp_base64_mt.o: packages/base64/sp_base64.c \
                                lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_compat.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(PKG_MT_FLAGS) -Ilib packages/base64/sp_base64.c -o $@

# tmpdir: Dir.tmpdir and Dir.mktmpdir, the system temp directory and a
# unique-directory creator. Pure C, no struct.
packages/tmpdir/sp_tmpdir.o: packages/tmpdir/sp_tmpdir.c \
                             lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_compat.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib packages/tmpdir/sp_tmpdir.c -o $@
packages/tmpdir/sp_tmpdir_mt.o: packages/tmpdir/sp_tmpdir.c \
                                lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_compat.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(PKG_MT_FLAGS) -Ilib packages/tmpdir/sp_tmpdir.c -o $@

# zlib: DEFLATE, zlib and gzip in the package's own C. No system libz and so
# no availability probe: a host with libz.so.1 and no zlib.h -- an ordinary
# box without the -dev package -- would drop the package and give a green
# `make test` that never ran it. Pure C, no struct.
packages/zlib/sp_zlib.o: packages/zlib/sp_zlib.c \
                         lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_compat.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib packages/zlib/sp_zlib.c -o $@
packages/zlib/sp_zlib_mt.o: packages/zlib/sp_zlib.c \
                            lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h lib/sp_compat.h
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(PKG_MT_FLAGS) -Ilib packages/zlib/sp_zlib.c -o $@

build/sp_cold.o: lib/sp_cold.c $(RT_HDRS)
	@mkdir -p build
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) -Ilib -Ilib/regexp -Ilib/regexp/shim lib/sp_cold.c -o build/sp_cold.o

SP_RT_LIB = lib/libspinel_rt.a

RT_MEMBERS = sp_bigint sp_crypto sp_pack sp_time sp_core sp_net sp_system sp_gc sp_slab sp_alloc sp_dtoa sp_marshal sp_format sp_string sp_inspect sp_array sp_str sp_str_crypt sp_hash sp_proc sp_exc sp_re sp_random sp_fiber sp_sched sp_io sp_iobuffer sp_cold sp_process sp_process_status

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

# Specific rule before generic: GNU Make 3.81 (macOS system make) picks the
# first matching pattern rule, not the shortest-stem one (3.82+).
build/mt/regexp/%.o: lib/regexp/%.c $(RE_HDRS)
	@mkdir -p $(@D)
	$(CC) -c $(COPT) $(SEC_FLAGS) $(MT_DEF) -Ilib/regexp -Ilib/regexp/shim $< -o $@

build/mt/%.o: lib/%.c $(RT_HDRS)
	@mkdir -p $(@D)
	$(CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(MT_DEF) -Ilib -Ilib/regexp -Ilib/regexp/shim $< -o $@

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
build/mt-tsan/regexp/%.o: lib/regexp/%.c $(RE_HDRS)
	@mkdir -p $(@D)
	$(CC) -c -O1 $(SEC_FLAGS) $(TSAN_DEF) -Ilib/regexp -Ilib/regexp/shim $< -o $@

build/mt-tsan/%.o: lib/%.c $(RT_HDRS)
	@mkdir -p $(@D)
	$(CC) -c -O1 -Wno-all $(SEC_FLAGS) $(TSAN_DEF) -Ilib -Ilib/regexp -Ilib/regexp/shim $< -o $@

RE_MT_TSAN_OBJ = $(patsubst lib/regexp/%.c,build/mt-tsan/regexp/%.o,$(RE_SRC))

$(SP_RT_MT_TSAN_LIB): $(RE_MT_TSAN_OBJ) $(addprefix build/mt-tsan/,$(addsuffix .o,$(RT_MEMBERS)))
	ar rcs $@ $^

tsan-archive: $(SP_RT_MT_TSAN_LIB)

# ---- wasm32-wasi runtime archive ----
# `spinel --target=wasm32-wasi` links lib/wasm32-wasi/libspinel_rt.a, the
# same sources compiled by the wasi-sdk's clang (WASI_SDK, or the sdk's own
# WASI_SDK_PATH, else /opt/wasi-sdk) with lib/wasi's stand-ins for the POSIX
# headers wasi-libc leaves out and the flags the driver passes the program
# (src/main.c: the same list, so the two halves of one module agree).
# Single-threaded only: wasm has no threads without SharedArrayBuffer. The
# bundled packages get a `<stem>_wasi.o` each, which the driver links in
# place of the native object; openssl is glue over a system libssl and has
# none. Not built by default: `make wasm-rt` on demand, and only where the
# sdk is.
WASI_SDK ?= $(if $(WASI_SDK_PATH),$(WASI_SDK_PATH),/opt/wasi-sdk)
WASI_CC = $(WASI_SDK)/bin/clang
WASI_AR = $(WASI_SDK)/bin/llvm-ar
WASI_CFLAGS = -Ilib/wasi -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID -D_WASI_EMULATED_MMAN \
              -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false
SP_RT_WASI_LIB = lib/wasm32-wasi/libspinel_rt.a
WASI_SHIM_HDRS = $(wildcard lib/wasi/*.h lib/wasi/sys/*.h)

build/wasm32-wasi/regexp/%.o: lib/regexp/%.c $(RE_HDRS)
	@mkdir -p $(@D)
	$(WASI_CC) -c $(COPT) $(SEC_FLAGS) $(RE_CASE_FLAGS) $(WASI_CFLAGS) -Ilib/regexp -Ilib/regexp/shim $< -o $@

build/wasm32-wasi/wasi/%.o: lib/wasi/%.c $(WASI_SHIM_HDRS)
	@mkdir -p $(@D)
	$(WASI_CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(WASI_CFLAGS) -Ilib $< -o $@

build/wasm32-wasi/%.o: lib/%.c $(RT_HDRS) $(WASI_SHIM_HDRS)
	@mkdir -p $(@D)
	$(WASI_CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(WASI_CFLAGS) -Ilib -Ilib/regexp -Ilib/regexp/shim $< -o $@

RE_WASI_OBJ = $(patsubst lib/regexp/%.c,build/wasm32-wasi/regexp/%.o,$(RE_SRC))
WASI_SHIM_OBJ = $(patsubst lib/wasi/%.c,build/wasm32-wasi/wasi/%.o,$(wildcard lib/wasi/*.c))

$(SP_RT_WASI_LIB): $(RE_WASI_OBJ) $(WASI_SHIM_OBJ) $(addprefix build/wasm32-wasi/,$(addsuffix .o,$(RT_MEMBERS)))
	@mkdir -p $(@D)
	rm -f $@ && $(WASI_AR) rcs $@ $^

BUNDLED_NATIVE_WASI_OBJS = $(patsubst %.o,%_wasi.o,$(filter-out packages/openssl/% packages/ffi/%,$(BUNDLED_NATIVE_OBJS)))
packages/%_wasi.o: packages/%.c lib/spinel/runtime.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h $(WASI_SHIM_HDRS)
	$(WASI_CC) -c $(COPT) -Wno-all $(SEC_FLAGS) $(WASI_CFLAGS) -Ilib -I$(@D) $< -o $@

wasm-rt: $(SP_RT_WASI_LIB) $(BUNDLED_NATIVE_WASI_OBJS)

# A few of the corpus's programs built with --target=wasm32-wasi and run
# under wasmtime (WASMTIME, default: the one on PATH) against their expected
# output: exceptions (the sjlj lowering), regexps, floats, a Bignum, a
# bundled package, a 32-bit Integer. Not in the gate: it needs the sdk and
# an engine. `make wasm-test` where both are.
WASMTIME ?= wasmtime
WASM_TESTS = test/string_gsub_block.rb test/rescue_roots_under_collection.rb test/float_round_half.rb \
             test/bignum_modulo_bit_pow.rb test/json_user_to_json_bytes.rb test/array_flatten_typed_elements.rb \
             test/string_to_i_overflow_raises.rb
# WASM_ENGINE_REQUIRED=1 makes a missing engine a failure rather than a skip:
# the one job whose whole purpose is to prove these programs still link and run
# cannot report that by exiting 0 with "skipped" (#4807). CI sets it.
wasm-test: $(SPINEL) wasm-rt
	@if ! command -v $(WASMTIME) >/dev/null 2>&1; then \
	  if [ -n "$(WASM_ENGINE_REQUIRED)" ]; then echo "wasm-test: FAIL (no $(WASMTIME), and WASM_ENGINE_REQUIRED is set)"; exit 1; fi; \
	  echo "wasm-test: skipped (no $(WASMTIME))"; exit 0; fi; \
	tmp=$$(mktemp -d /tmp/spinel-wasm.XXXXXX); ok=1; \
	for t in $(WASM_TESTS); do \
	  bn=$$(basename $$t .rb); \
	  if ! WASI_SDK=$(WASI_SDK) $(SPINEL) --target=wasm32-wasi --no-line-map $$t -o $$tmp/$$bn.wasm >/dev/null 2>$$tmp/$$bn.build; then \
	    echo "wasm-test: FAIL (build) $$t"; head -3 $$tmp/$$bn.build; ok=0; continue; fi; \
	  $(WASMTIME) run -W max-wasm-stack=16777216 --dir=. $$tmp/$$bn.wasm >$$tmp/$$bn.out 2>$$tmp/$$bn.err </dev/null; \
	  if cmp -s $$tmp/$$bn.out $$t.expected; then echo "wasm-test: pass $$t"; \
	  else echo "wasm-test: FAIL $$t"; diff -u $$t.expected $$tmp/$$bn.out | head -6; head -3 $$tmp/$$bn.err; ok=0; fi; \
	done; \
	rm -rf $$tmp; [ $$ok -eq 1 ]

regexp: $(SP_RT_LIB) $(SP_RT_MT_LIB)

# ---- In-tree developer tools ----

# spinel-doctor / spinel-reduce / spinel-flatten: written in the spinel subset
# and compiled by spinel itself (dogfood), so their only runtime dependency is
# cc -- the same as the compiler. Each tools/<name>.rb becomes bin/spinel-<name>,
# beside the compiler, so the `spinel-<name>` command is found next to `spinel`.
# A tool that no longer fits the subset breaks the build, which keeps them honest.
TOOL_NAMES = doctor reduce flatten diff
TOOL_BINS  = $(addprefix bin/spinel-,$(TOOL_NAMES))

tools: $(TOOL_BINS) bin/spin

# spin: the project tool (self-hosted; see docs/spin.md). Stages the RBS
# extractor beside the compiler when vendor/rbs is fetched: a spin-driven
# build resolves --rbs via <dir-of-spinel>/spinel_rbs_extract, and without
# the copy it silently lost every .rbs seed (#1845 bounce 6).
# spin's own identity for `spin --version`: the same release and revision
# stamp the compiler carries (build/csrc/spinel_rev.h), rendered as a Ruby
# file spin requires, so the two say the same thing when they ship together.
# cmp-guarded like the header so only a HEAD move rebuilds spin.
build/spin_version.rb: build/csrc/spinel_rev.h
	@t=$@.tmp.$$$$; \
	{ echo "SPIN_RELEASE = \"$$(sed -n 's/^#define SPINEL_RELEASE "\(.*\)"/\1/p' build/csrc/spinel_rev.h)\""; \
	  echo "SPIN_BUILD_REV = \"$$(sed -n 's/^#define SPINEL_BUILD_REV "\(.*\)"/\1/p' build/csrc/spinel_rev.h)\""; } > $$t; \
	if cmp -s $$t $@; then rm -f $$t; else mv $$t $@; fi

bin/spin: tools/spin.rb tools/spin/toml.rb build/spin_version.rb $(SPINEL) $(SP_RT_LIB) $(SP_RT_MT_LIB) $(RBS_EXTRACT_TARGET)
	$(SPINEL) -I build tools/spin.rb -o bin/spin
	@if [ -n "$(RBS_EXTRACT_TARGET)" ]; then \
	  cp -f $(RBS_EXTRACT_BIN) bin/spinel_rbs_extract; \
	  echo "$(RBS_EXTRACT_BIN) -> bin/spinel_rbs_extract"; \
	fi

bin/spinel-%: tools/%.rb tools/tool_common.rb $(SPINEL) $(SP_RT_LIB) $(SP_RT_MT_LIB)
	@mkdir -p bin
	$(SPINEL) $< -o $@

# ---- Test ----

TESTS := $(wildcard test/*.rb)
# Build-incompatible: regexp_unicode_casefold and regexp_unicode_ctype pin
# what the Unicode case and type tables answer, and either RE_NO_UNICODE_*
# switch is the ASCII build that leaves both out.
ifneq (,$(findstring RE_NO_UNICODE_,$(RE_CASE_FLAGS)))
TESTS := $(filter-out test/regexp_unicode_casefold.rb test/regexp_unicode_ctype.rb,$(TESTS))
endif
# Mode-incompatible: int_overflow_raises pins raise-mode semantics; under
# --int-overflow=promote the same code auto-promotes and output diverges.
# float_to_int_out_of_range and float_to_int_boundary are the same case for
# the Float -> Integer conversions: they pin the RangeError raise mode keeps,
# which promote answers as a Bignum instead (#4688). The promote answers are
# pinned by promote_float_to_int.rb. str_to_i_overflow,
# string_to_i_overflow_raises and integer_argument_error's LLONG_MIN line pin
# the RangeError String#to_i and Integer() answer past sp_int in raise mode;
# promote reads a Bignum there, pinned by promote_str_to_i_bigint.rb.
# poly_call_legacy_abi_gate / poly_call_fast_abi_gate pin the raise/wrap legacy
# sp_int Method ABI classification; under promote the poly-ABI stamp gates the
# same dynamic calls instead, and the two classifications legitimately diverge
# in both directions: a target promote leaves mixed-signatured (a String or
# pointer PARAMETER beside boxed ones) declines there where legacy accepts it,
# while a pointer argument into an untyped parameter is callable there (both
# slots boxed) where legacy declines. The poly-slot promote behavior is pinned
# by promote_poly_slot_method_call instead. poly_method_return_kinds
# additionally trips a typed `.to_proc`-with-defaults promote gap (an IntArray
# default in a poly-widened callee), unrelated to the dispatch these pin.
ifeq ($(SPINEL_INT_OVERFLOW),promote)
TESTS := $(filter-out test/int_overflow_raises.rb test/int_overflow_op_assign.rb test/poly_int_overflow_raises.rb test/str_to_i_overflow.rb test/string_to_i_overflow_raises.rb test/integer_argument_error.rb test/bounded_counter_unchecked_add.rb test/float_to_int_out_of_range.rb test/bigrational_to_i_out_of_range.rb test/float_to_int_boundary.rb test/poly_call_legacy_abi_gate.rb test/poly_call_fast_abi_gate.rb test/poly_method_return_kinds.rb,$(TESTS))
# Drive the spinel front-end and the C compile in promote mode so the test
# rule actually exercises the auto-promotion path end to end.
SP_OV_FLAG := --int-overflow=promote
SP_OV_DEFINE := -DSP_INT_OVERFLOW_MODE_PROMOTE
else
# `promote_*` tests overflow on purpose and only have defined output under
# --int-overflow=promote; in raise/wrap mode they would (correctly) raise.
TESTS := $(filter-out test/promote_%.rb,$(TESTS))
endif
# A 32-bit target has a 32-bit Integer (lib/sp_types.h): a test that assumes
# the 64-bit one (values or arithmetic past 2^31, `Integer#size == 8`, a
# printed hash value, a 64-bit FFI width) says so in its first line and is
# not run there. `make CC='cc -m32'` on a 64-bit host is such a target.
ifeq ($(SPINEL_INT_BITS),32)
TESTS := $(filter-out $(shell grep -l '^\# spinel: int64' test/*.rb),$(TESTS))
endif
# TEST_SHARD=k/n runs the k-th of n slices of the corpus (1-based), the
# slice taken by position in the sorted list so every test lands in exactly
# one: CI runs the slices as parallel jobs, since the corpus is what the
# jobs' wall time is made of. The bundled packages' tests are sliced the
# same way below. Unset, the whole corpus runs.
#
# The pick is make's own: the shell is asked only for the positions (k, k+n,
# k+2n, ...), and $(word) takes those. The list itself used to go through the
# shell, `printf '%s\n' <every test> | awk ...`, as one `sh -c` string. Past
# 128 KiB, Linux refuses a single argument that long (MAX_ARG_STRLEN), so
# once the corpus names outgrew it (late September 2026, about 4,200 tests)
# make printed "/bin/sh: Argument list too long" and the pick answered
# nothing. Every CI lane then ran only the packages' tests and still passed.
# macOS has no such per-argument limit, so a local run never showed it.
ifneq ($(TEST_SHARD),)
SHARD_K := $(word 1,$(subst /, ,$(TEST_SHARD)))
SHARD_N := $(word 2,$(subst /, ,$(TEST_SHARD)))
shard_pick = $(foreach i,$(shell seq $(SHARD_K) $(SHARD_N) $(words $(1))),$(word $(i),$(1)))
SHARD_ALL := $(TESTS)
TESTS := $(call shard_pick,$(TESTS))
# A slice that comes out empty from a non-empty corpus means the pick
# failed. Stop, rather than let a green run test nothing.
ifneq ($(SHARD_ALL),)
ifeq ($(TESTS),)
$(error TEST_SHARD=$(TEST_SHARD) picked no test out of $(words $(SHARD_ALL)))
endif
endif
endif
TEST_TARGETS := $(patsubst test/%.rb,build/test-results/%.ok,$(TESTS))

# Bundled spin packages carry their own test/*.rb (the same snapshot contract,
# runnable with `spin test` inside the package). The compiler gate runs them
# too -- bundled packages are versioned with the compiler, so a compiler change
# that breaks one must fail here, not at package-publish time. Targets are
# namespaced pkg.<package>.<test>.ok to avoid colliding with test/ names.
PKG_TESTS := $(wildcard packages/*/test/*.rb)
# The openssl package is glue over the SYSTEM libssl, so its tests only exist
# where the headers do -- the object drops out of BUNDLED_NATIVE_OBJS on the
# same probe, and `require "openssl"` is then an unsatisfiable require.
ifneq ($(OPENSSL_AVAILABLE),yes)
PKG_TESTS := $(filter-out packages/openssl/test/%.rb,$(PKG_TESTS))
endif
ifneq ($(FFI_AVAILABLE),yes)
PKG_TESTS := $(filter-out packages/ffi/test/%.rb packages/fiddle/test/%.rb,$(PKG_TESTS))
endif
ifeq ($(SPINEL_INT_BITS),32)   # the same first-line marker as test/*.rb
PKG_TESTS := $(filter-out $(shell grep -l '^\# spinel: int64' packages/*/test/*.rb),$(PKG_TESTS))
endif
ifneq ($(TEST_SHARD),)
PKG_TESTS := $(call shard_pick,$(PKG_TESTS))
endif
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
# compiler kind, $(OPT) and the overflow mode because a PCH only loads under
# the exact flags it was built with. The mode was missing from the key, and
# a -D mismatch is one clang accepts SILENTLY: after a raise-mode run, the
# promote suite reused the raise-mode .gch and every test ran against
# raise-semantics runtime inlines -- the mode-sensitive tests failed and the
# rest tested the wrong runtime without a word (#4538).
CC_KIND  := $(if $(findstring clang,$(shell $(CC) --version 2>/dev/null | head -1)),clang,gcc)
# The key has to be ONE path component: $(OPT) is a flag LIST, so a
# multi-flag setting (`COPT := -O2 -g0` in config.mk) put a space in the
# middle and every use of PCH_ROOT then split into two words -- two make
# targets, an -I pointing at the wrong directory, and a mkdir of /plain
# (#4256). Strip the characters that cannot appear in one component:
# the dashes the key never wanted, then spaces, slashes and equals.
sp_empty :=
sp_space := $(sp_empty) $(sp_empty)
sp_pathify = $(subst =,,$(subst /,,$(subst $(sp_space),,$(subst -,,$(1)))))
PCH_ROOT := build/pch/$(CC_KIND)$(call sp_pathify,$(OPT))$(if $(SP_OV_DEFINE),promote)
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

# A regexp literal is two constants, the pattern and its flags, so whether the
# engine can read it is settled at compile time. It used to reach the engine
# only at the built program's startup, so a pattern like /[z-a]/ built clean
# and raised RegexpError when the program ran; CRuby reports it from the parse.
# There is no place for this in test/*.rb, whose harness needs the compile to
# succeed, so the refusal is checked here.
re-lit-test: $(SPINEL)
	@tmp=$$(mktemp -d /tmp/spinel-relit.XXXXXX); ok=1; \
	printf 'p(/[z-a]/i)\n' > "$$tmp/bad.rb"; \
	if $(SPINEL) "$$tmp/bad.rb" -o "$$tmp/bad" >"$$tmp/bad.out" 2>&1; then \
	  echo "re-lit-test: FAIL (a literal the engine cannot read still built)"; ok=0; \
	else \
	  grep -q 'bad.rb:1: empty range in char class: /\[z-a\]/i' "$$tmp/bad.out" || \
	    { echo "re-lit-test: FAIL (the refusal did not name the line, the pattern and its flags)"; cat "$$tmp/bad.out"; ok=0; }; \
	fi; \
	printf 'x = 1\ny = 2\np(/(?<1>a)/)\n' > "$$tmp/name.rb"; \
	if $(SPINEL) "$$tmp/name.rb" -o "$$tmp/name" >"$$tmp/name.out" 2>&1; then \
	  echo "re-lit-test: FAIL (an invalid group name still built)"; ok=0; \
	else \
	  grep -q 'name.rb:3: invalid group name <1>' "$$tmp/name.out" || \
	    { echo "re-lit-test: FAIL (the refusal named the wrong line)"; cat "$$tmp/name.out"; ok=0; }; \
	fi; \
	printf 'p("m" =~ /[a-z]/)\np("M" =~ /[a-z]/i)\n' > "$$tmp/good.rb"; \
	if $(SPINEL) "$$tmp/good.rb" -o "$$tmp/good" >"$$tmp/good.out" 2>&1; then \
	  [ "$$("$$tmp/good")" = "$$(printf '0\n0')" ] || { echo "re-lit-test: FAIL (a valid literal changed behaviour)"; "$$tmp/good"; ok=0; }; \
	else echo "re-lit-test: FAIL (a valid literal was refused)"; cat "$$tmp/good.out"; ok=0; fi; \
	printf 'x = "z-a"\nre = /[#{x}]/\np((("m" =~ re) ? 1 : 0))\n' > "$$tmp/interp.rb"; \
	if $(SPINEL) "$$tmp/interp.rb" -o "$$tmp/interp" >"$$tmp/interp.out" 2>&1; then \
	  "$$tmp/interp" >"$$tmp/interp.run" 2>&1 || true; \
	  grep -q 'empty range in char class' "$$tmp/interp.run" || \
	    { echo "re-lit-test: FAIL (an interpolated pattern should stay a runtime question)"; cat "$$tmp/interp.run"; ok=0; }; \
	else echo "re-lit-test: FAIL (an interpolated pattern was refused at compile time)"; cat "$$tmp/interp.out"; ok=0; fi; \
	rm -rf "$$tmp"; \
	if [ $$ok = 1 ]; then echo "re-lit-test: pass"; else exit 1; fi

# `make test` always runs fresh: it wipes the prior `.ok` stamps first,
# then runs the suite. (The old incremental `test` + `retest` split is
# gone -- a stale `.ok` reading PASS was a recurring foot-gun.)
test: $(SPINEL_TIMEOUT)
	@if [ -z "$(TIMEOUT_BIN)" ]; then \
	  echo "WARNING: no 'timeout'/'gtimeout' on PATH -- tests run with NO time limit."; \
	  echo "         A hanging test will hang this run until the CI job's own limit."; \
	fi
	+@$(MAKE) --no-print-directory clean-test-results
	+@$(MAKE) $(TEST_JOBS) --no-print-directory test-run

# The actual run. rbs-test golden-checks the RBS extractor (cheap, C-only).
# rbs-seed-test checks the seeds actually reach the analyzer (incl. nested
# classes, #1417).
test-run: rbs-test rbs-seed-test re-lit-test reject-test cli-opts-test defer-refusals-test check-stores-test backtrace-test gc-minor-test gc-phases-test gc-threshold-test gc-obj-budget-test gc-str-major-test threaded-render-test gc-locality-test byref-capture-test thread-puts-test ext-test ext-cruby-test test-corpus-summary

# The test/*.rb corpus (and the bundled packages') on its own, without the
# C-side legs: what a 32-bit target runs (`make test-corpus CC='cc -m32'`),
# whose CRuby extension leg and rbs tooling have no 32-bit toolchain to
# build against.
test-corpus: $(SPINEL_TIMEOUT)
	+@$(MAKE) --no-print-directory clean-test-results
	+@$(MAKE) $(TEST_JOBS) --no-print-directory test-corpus-summary
test-corpus-summary: $(TEST_TARGETS) $(PKG_TEST_TARGETS)
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

# ---- Rejection diagnostics ----
# A construct spinel deliberately does not compile must name itself, at the
# Ruby line that has it. Falling through to the C compiler reports generated
# code the author never wrote (#4169).
# ext-test: the Layer-1 extension emission (docs/internals/ext-design.md):
# --ext-init/--ext-entry compile a kernel into a host-callable library, and a
# pure-C host drives it through the emitted header alone -- init, typed
# entries, and a raise caught through the exported try helper.
ext-test: $(SPINEL) $(SP_RT_LIB)
	@tmp=$$(mktemp -d /tmp/spinel-ext.XXXXXX); ok=1; \
	$(SPINEL) test/ext/kernel.rb -c --no-line-map \
	  --ext-init Init_ext_kernel \
	  --ext-entry ExtKernel.triple,ExtKernel.shout,ExtKernel.total,ExtKernel.must_pos \
	  -o "$$tmp/k.c" >/dev/null 2>&1 || { echo "ext-test: FAIL (emission)"; ok=0; }; \
	printf 'module M\n  def self.eat(a)\n    a.sort!\n  end\nend\nif __FILE__ == $$0\n  M.eat([2, 1])\nend\n' > "$$tmp/mut.rb"; \
	if $(SPINEL) "$$tmp/mut.rb" -c --no-line-map --ext-init spx_i --ext-entry M.eat -o "$$tmp/m.c" >"$$tmp/m.out" 2>&1; then \
	  echo "ext-test: FAIL (a parameter mutation compiled, R4)"; ok=0; \
	else grep -q "mutates its parameter" "$$tmp/m.out" || { echo "ext-test: FAIL (R4 refused without saying why)"; sed -n 1,3p "$$tmp/m.out"; ok=0; }; fi; \
	if [ $$ok -eq 1 ]; then \
	  grep -q "int main" "$$tmp/k.c" && { echo "ext-test: FAIL (main leaked into the library)"; ok=0; }; \
	  grep -q "toplevel ran" "$$tmp/k.c" || { echo "ext-test: FAIL (toplevel missing from init)"; ok=0; }; \
	fi; \
	if [ $$ok -eq 1 ]; then \
	  if $(CC) -O1 -w -Ilib -I"$$tmp" test/ext/host.c "$$tmp/k.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/host" 2>"$$tmp/cc.err"; then \
	    "$$tmp/host" > "$$tmp/out" 2>&1; \
	    cmp -s "$$tmp/out" test/ext/expected || { echo "ext-test: FAIL (host output mismatch)"; diff -u test/ext/expected "$$tmp/out" || true; ok=0; }; \
	  else echo "ext-test: FAIL (host C did not compile)"; sed -n 1,6p "$$tmp/cc.err"; ok=0; fi; \
	fi; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "ext-test: pass"; else exit 1; fi

# ext-cruby-test: Layer 2 (--ext cruby): the generated shim compiles into a
# real .so and a CRuby driver runs it -- values, boundary TypeError, a kernel
# raise crossing as ArgumentError, and the toplevel constant visible through
# the fallback-shaped require. Skips cleanly without ruby dev headers.
ext-cruby-test: $(SPINEL) $(SP_RT_LIB)
	@if ! command -v ruby >/dev/null 2>&1; then echo "ext-cruby-test: skipped (no ruby)"; exit 0; fi; \
	RH=$$(ruby -e 'puts RbConfig::CONFIG["rubyhdrdir"]' 2>/dev/null); \
	RA=$$(ruby -e 'puts RbConfig::CONFIG["rubyarchhdrdir"]' 2>/dev/null); \
	DLEXT=$$(ruby -e 'puts RbConfig::CONFIG["DLEXT"]' 2>/dev/null); \
	if [ ! -f "$$RH/ruby.h" ]; then echo "ext-cruby-test: skipped (no ruby.h)"; exit 0; fi; \
	if [ "$$(uname -s)" = Darwin ]; then SOFLAGS="-bundle -Wl,-undefined,dynamic_lookup"; else SOFLAGS="-shared"; fi; \
	tmp=$$(mktemp -d /tmp/spinel-extrb.XXXXXX); ok=1; \
	$(SPINEL) test/ext/kernel.rb -c --no-line-map --ext cruby \
	  --ext-init spx_init_extk \
	  --ext-entry ExtKernel.triple,ExtKernel.shout,ExtKernel.total,ExtKernel.pair_sum,ExtKernel.must_pos \
	  -o "$$tmp/extk.c" >/dev/null 2>&1 || { echo "ext-cruby-test: FAIL (emission)"; ok=0; }; \
	if [ $$ok -eq 1 ]; then \
	  if $(CC) $$SOFLAGS -fPIC -O1 -w -I"$$RH" -I"$$RA" -Ilib -Ilib/regexp -Ilib/regexp/shim -I"$$tmp" \
	       "$$tmp/extk_ext.c" "$$tmp/extk.c" $$(ls lib/*.c lib/regexp/*.c | sed 's/^/ /') \
	       $(LDFLAGS) -lm -o "$$tmp/extk.$$DLEXT" 2>"$$tmp/cc.err"; then \
	    ( cd "$$tmp" && cp $(CURDIR)/test/ext/driver.rb . && ruby driver.rb > out 2>&1 ); \
	    cmp -s "$$tmp/out" test/ext/expected_cruby || { echo "ext-cruby-test: FAIL (driver output mismatch)"; diff -u test/ext/expected_cruby "$$tmp/out" || true; ok=0; }; \
	  else echo "ext-cruby-test: FAIL (.so did not compile)"; sed -n 1,6p "$$tmp/cc.err"; ok=0; fi; \
	fi; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "ext-cruby-test: pass"; else exit 1; fi

# An option spinel does not know is a mistake, and building something other
# than what was asked for is the one thing it must not do quietly. Also pins
# the joined -O<n> spelling, which every C compiler takes and which used to
# fall through to the unknown-flag arm and be discarded.
# --defer-refusals: the program the compiler refuses builds anyway; a refused
# method raises NotImplementedError when called, a refused top-level or
# class-body statement is left out.
defer-refusals-test: $(SPINEL)
	@ok=1; tmp=$$(mktemp -d /tmp/spinel-defer.XXXXXX); \
	for spec in "deferred_refusals:2:top NotImplementedError true done " \
	            "deferred_refusal_class_body:1:before "; do \
	  t=test/defer/$${spec%%:*}.rb; rest=$${spec#*:}; n=$${rest%%:*}; want=$${rest#*:}; \
	  if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/p.c" >"$$tmp/p.out" 2>&1; then \
	    echo "defer-refusals-test: FAIL ($$t compiled without the flag)"; ok=0; fi; \
	  if ! $(SPINEL) --defer-refusals "$$t" -o "$$tmp/d" >"$$tmp/d.out" 2>&1; then \
	    echo "defer-refusals-test: FAIL ($$t: --defer-refusals still refused)"; sed -n 1,5p "$$tmp/d.out"; ok=0; continue; fi; \
	  if [ $$n -eq 1 ]; then rn="1 refusal deferred"; else rn="$$n refusals deferred"; fi; \
	  grep -q "$$rn to run time" "$$tmp/d.out" || \
	    { echo "defer-refusals-test: FAIL ($$t: deferred refusals not counted)"; sed -n 1,5p "$$tmp/d.out"; ok=0; }; \
	  "$$tmp/d" >"$$tmp/r.out" 2>"$$tmp/r.err"; st=$$?; out=$$(tr '\n' ' ' <"$$tmp/r.out"); \
	  [ "$$out" = "$$want" ] || { echo "defer-refusals-test: FAIL ($$t ran wrong: $$out)"; ok=0; }; \
	  [ $$st -ne 0 ] && grep -q "unicode_normalize is not supported.*(NotImplementedError)" "$$tmp/r.err" || \
	    { echo "defer-refusals-test: FAIL ($$t: a refused line did not raise NotImplementedError, exit $$st)"; sed -n 1,3p "$$tmp/r.err"; ok=0; }; \
	done; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "defer-refusals-test: pass"; else exit 1; fi

# --check-stores: each value the emitter writes as it is into a C slot of
# another C type is reported at its Ruby line, and the C is the same as without
# the flag but for the comments marking those stores. raw_stores.rb holds the
# stores master still writes raw; clean_stores.rb, stores that convert.
check-stores-test: $(SPINEL)
	@ok=1; tmp=$$(mktemp -d /tmp/spinel-stores.XXXXXX); t=test/check-stores/raw_stores.rb; \
	$(SPINEL) --check-stores "$$t" -c -o "$$tmp/k.c" >"$$tmp/k.out" 2>&1 || { echo "check-stores-test: FAIL ($$t did not compile)"; ok=0; }; \
	n=$$(grep -c 'store check' "$$tmp/k.out"); [ "$$n" -le 1 ] || { echo "check-stores-test: FAIL ($$n reports, want at most 1)"; ok=0; }; \
	$(SPINEL) "$$t" -c -o "$$tmp/p.c" >/dev/null 2>&1; \
	sed 's|/\* store check: [^*]* \*/||g' "$$tmp/k.c" | cmp -s - "$$tmp/p.c" || { echo "check-stores-test: FAIL (the flag changed the C beyond its comments)"; ok=0; }; \
	$(SPINEL) --check-stores test/check-stores/clean_stores.rb -c -o "$$tmp/c.c" >"$$tmp/c.out" 2>&1 || \
	  { echo "check-stores-test: FAIL (clean_stores.rb did not compile)"; sed -n 1,5p "$$tmp/c.out"; ok=0; }; \
	! grep -q 'store check' "$$tmp/c.out" || { echo "check-stores-test: FAIL (a store that converts was reported)"; grep 'store check' "$$tmp/c.out"; ok=0; }; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "check-stores-test: pass"; else exit 1; fi

cli-opts-test: $(SPINEL)
	@ok=1; tmp=$$(mktemp -d /tmp/spinel-cliopts.XXXXXX); \
	printf 'p ARGV\n' > "$$tmp/p.rb"; \
	for f in --no-such-flag --no-inline-hott -Q; do \
	  if $(SPINEL) "$$f" "$$tmp/p.rb" -c -o "$$tmp/o.c" >"$$tmp/o.out" 2>&1; then \
	    echo "cli-opts-test: FAIL ($$f was accepted)"; ok=0; \
	  else grep -qF "unknown option '$$f'" "$$tmp/o.out" || \
	    { echo "cli-opts-test: FAIL ($$f refused without naming it)"; sed -n 1,3p "$$tmp/o.out"; ok=0; }; fi; \
	done; \
	printf '#!/bin/sh\necho "$$@" >> "$$0.args"\nexec cc "$$@"\n' > "$$tmp/ccwrap"; \
	chmod +x "$$tmp/ccwrap"; \
	for o in 0 1 2; do \
	  : > "$$tmp/ccwrap.args"; \
	  $(SPINEL) -O$$o --cc="$$tmp/ccwrap" "$$tmp/p.rb" -o "$$tmp/j$$o" >"$$tmp/j.out" 2>&1 || \
	    { echo "cli-opts-test: FAIL (-O$$o joined form refused)"; sed -n 1,3p "$$tmp/j.out"; ok=0; continue; }; \
	  grep -qe "-O$$o" "$$tmp/ccwrap.args" || \
	    { echo "cli-opts-test: FAIL (-O$$o accepted but the C compiler was not given it)"; \
	      cat "$$tmp/ccwrap.args"; ok=0; }; \
	done; \
	out=$$($(SPINEL) -E "$$tmp/p.rb" --a-program-flag 2>&1); \
	[ "$$out" = '["--a-program-flag"]' ] || \
	  { echo "cli-opts-test: FAIL (run mode did not hand the program its flag: $$out)"; ok=0; }; \
	$(SPINEL) -g test/debug/ivar_nil_before_setup.rb -o "$$tmp/dbg" >"$$tmp/dbg.out" 2>&1 && \
	  "$$tmp/dbg" 2>&1 | cmp -s - test/debug/ivar_nil_before_setup.rb.expected || \
	  { echo "cli-opts-test: FAIL (a -g build did not raise NoMethodError for an unset ivar, #5960)"; ok=0; }; \
	$(SPINEL) -I test/require_load_path test/require_load_path/main.rb -o "$$tmp/lp" >"$$tmp/lp.out" 2>&1 && \
	  "$$tmp/lp" 2>&1 | cmp -s - test/require_load_path/main.rb.expected || \
	  { echo "cli-opts-test: FAIL (a file reached by -I require and by require_relative loaded twice)"; ok=0; }; \
	links=""; i=0; while [ $$i -lt 70 ]; do links="$$links --link -lm"; i=$$((i + 1)); done; \
	$(SPINEL) "$$tmp/p.rb" $$links --link -lsp_last_link --print-build 2>/dev/null | grep -q 'lib -lsp_last_link' || \
	  { echo "cli-opts-test: FAIL (a --link past the 64th was dropped)"; ok=0; }; \
	mkdir "$$tmp/feats"; : > "$$tmp/feats.rb"; i=0; while [ $$i -lt 130 ]; do \
	  echo "F$$i = $$i" > "$$tmp/feats/f$$i.rb"; echo "require \"f$$i\"" >> "$$tmp/feats.rb"; i=$$((i + 1)); done; \
	printf 'require "ostruct"\nputs OpenStruct.new(a: 1).a\n' >> "$$tmp/feats.rb"; \
	$(SPINEL) -I "$$tmp/feats" --require-gate "$$tmp/feats.rb" -c -o "$$tmp/feats.c" >"$$tmp/feats.out" 2>&1 || \
	  { echo "cli-opts-test: FAIL (a require past the 128th feature was not recorded)"; sed -n 1,3p "$$tmp/feats.out"; ok=0; }; \
	incs=""; i=0; while [ $$i -lt 70 ]; do mkdir -p "$$tmp/roots/r$$i"; incs="$$incs -I $$tmp/roots/r$$i"; i=$$((i + 1)); done; \
	echo 'puts "deep root"' > "$$tmp/roots/r69/deep_root.rb"; echo 'require "deep_root"' > "$$tmp/dr.rb"; \
	$(SPINEL) $$incs "$$tmp/dr.rb" -o "$$tmp/dr" >"$$tmp/dr.out" 2>&1 && [ "$$("$$tmp/dr")" = "deep root" ] || \
	  { echo "cli-opts-test: FAIL (an -I root past the 64th was dropped)"; sed -n 1,3p "$$tmp/dr.out"; ok=0; }; \
	printf 'puts "hello"\n' > "$$tmp/hello.rb"; printf 'puts "ab".crypt("ab")\n' > "$$tmp/crypt.rb"; \
	if $(SPINEL) "$$tmp/hello.rb" --print-build 2>&1 | grep -q -- "-lcrypt"; then \
	  echo "cli-opts-test: FAIL (a program without String#crypt links libcrypt)"; ok=0; fi; \
	if [ "$$(uname)" = Linux ] && ! $(SPINEL) "$$tmp/crypt.rb" --print-build 2>&1 | grep -q -- "-lcrypt"; then \
	  echo "cli-opts-test: FAIL (String#crypt does not link libcrypt)"; ok=0; fi; \
	rm -rf "$$tmp"; \
	[ $$ok = 1 ] && echo "cli-opts-test: pass" || exit 1

reject-test: $(SPINEL)
	@ok=1; tmp=$$(mktemp -d /tmp/spinel-reject.XXXXXX); \
	for t in test/reject/string_thread_arg.rb test/reject/string_fiber_arg.rb test/reject/string_thread_global_arg.rb test/reject/string_thread_ivar_arg.rb test/reject/string_thread_method_param_arg.rb test/reject/string_fiber_method_param_arg.rb test/reject/string_thread_block_param_arg.rb test/reject/string_thread_arg_in_loop.rb; do \
	  if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/sk.c" >"$$tmp/sk.out" 2>&1; then \
	    echo "reject-test: FAIL ($$t compiled)"; ok=0; \
	  else grep -q "is not yet shared by reference" "$$tmp/sk.out" || \
	    { echo "reject-test: FAIL ($$t refused without saying why)"; sed -n 1,5p "$$tmp/sk.out"; ok=0; }; fi; \
	done; \
	for t in test/reject/string_hash_value_variable.rb test/reject/string_hash_pair_variable.rb test/reject/string_hash_values_variable.rb test/reject/string_hash_literal_captured.rb \
	         test/reject/string_hash_store_value.rb test/reject/string_hash_store_pair.rb \
	         test/reject/string_hash_store_value_block.rb test/reject/string_hash_store_pair_block.rb \
	         test/reject/string_hash_fresh.rb \
	         test/reject/string_hash_fresh_each.rb \
	         test/reject/string_hash_fresh_pair.rb \
	         test/reject/string_hash_fresh_values.rb \
	         test/reject/string_hash_interpolated.rb \
	         test/reject/string_hash_call.rb \
	         test/reject/string_hash_fresh_store.rb \
	         test/reject/string_hash_fresh_store_block.rb \
	         test/reject/string_hash_fresh_index.rb \
	         test/reject/string_hash_fresh_literal.rb; do \
	  if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/sk.c" >"$$tmp/sk.out" 2>&1; then \
	    echo "reject-test: FAIL ($$t compiled)"; ok=0; \
	  else grep -q "is not yet shared by reference" "$$tmp/sk.out" || \
	    { echo "reject-test: FAIL ($$t refused without saying why)"; sed -n 1,5p "$$tmp/sk.out"; ok=0; }; fi; \
	done; \
	for t in test/reject/string_yield_captured_param.rb test/reject/string_yield_splat_captured.rb; do \
	  if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/sk.c" >"$$tmp/sk.out" 2>&1; then \
	    echo "reject-test: FAIL ($$t compiled)"; ok=0; \
	  else grep -q "is not yet shared by reference" "$$tmp/sk.out" || \
	    { echo "reject-test: FAIL ($$t refused without saying why)"; sed -n 1,5p "$$tmp/sk.out"; ok=0; }; fi; \
	done; \
	t=test/reject/string_chained_index_append.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/r.c" >"$$tmp/r.out" 2>&1; then \
	  echo "reject-test: FAIL (string_chained_index_append compiled)"; ok=0; \
	else grep -q "is not yet shared by reference" "$$tmp/r.out" || \
	  { echo "reject-test: FAIL (string_chained_index_append rejected without saying why)"; head -5 "$$tmp/r.out"; ok=0; }; fi; \
	t=test/reject/scrub_bang_retained_append.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/r.c" >"$$tmp/r.out" 2>&1; then \
	  echo "reject-test: FAIL (scrub_bang_retained_append compiled)"; ok=0; \
	else grep -q "is not yet shared by reference" "$$tmp/r.out" || \
	  { echo "reject-test: FAIL (scrub_bang_retained_append rejected without saying why)"; head -5 "$$tmp/r.out"; ok=0; }; fi; \
	t=test/reject/scrub_bang_block.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/r.c" >"$$tmp/r.out" 2>&1; then \
	  echo "reject-test: FAIL (scrub_bang_block compiled)"; ok=0; \
	else grep -q "scrub! with a block" "$$tmp/r.out" || \
	  { echo "reject-test: FAIL (scrub_bang_block rejected without saying why)"; head -5 "$$tmp/r.out"; ok=0; }; fi; \
	t=test/reject/string_ivar_array_append.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/r.c" >"$$tmp/r.out" 2>&1; then \
	  echo "reject-test: FAIL (string_ivar_array_append compiled)"; ok=0; \
	else grep -q "is not yet shared by reference" "$$tmp/r.out" || \
	  { echo "reject-test: FAIL (string_ivar_array_append rejected without saying why)"; head -5 "$$tmp/r.out"; ok=0; }; fi; \
	t=test/reject/string_fresh_array_append.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/r.c" >"$$tmp/r.out" 2>&1; then \
	  echo "reject-test: FAIL (string_fresh_array_append compiled)"; ok=0; \
	else grep -q "is not yet shared by reference" "$$tmp/r.out" || \
	  { echo "reject-test: FAIL (string_fresh_array_append rejected without saying why)"; head -5 "$$tmp/r.out"; ok=0; }; fi; \
	t=test/reject/string_tap_fresh_append.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/r.c" >"$$tmp/r.out" 2>&1; then \
	  echo "reject-test: FAIL (string_tap_fresh_append compiled)"; ok=0; \
	else grep -q "is not yet shared by reference" "$$tmp/r.out" || \
	  { echo "reject-test: FAIL (string_tap_fresh_append rejected without saying why)"; head -5 "$$tmp/r.out"; ok=0; }; fi; \
	t=test/reject/string_narrowed_element_append.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/r.c" >"$$tmp/r.out" 2>&1; then \
	  echo "reject-test: FAIL (string_narrowed_element_append compiled)"; ok=0; \
	else grep -q "is not yet shared by reference" "$$tmp/r.out" || \
	  { echo "reject-test: FAIL (string_narrowed_element_append rejected without saying why)"; head -5 "$$tmp/r.out"; ok=0; }; fi; \
	t=test/reject/singleton_on_untraceable_recv.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/r.c" >"$$tmp/r.out" 2>&1; then \
	  echo "reject-test: FAIL (a singleton def on an untraceable receiver compiled)"; ok=0; \
	else grep -q "singleton method that needs a self, on a receiver that is not one user-class instance" "$$tmp/r.out" || \
	  { echo "reject-test: FAIL (rejected without saying why)"; sed -n 1,5p "$$tmp/r.out"; ok=0; }; fi; \
	t=test/reject/new_receiver_class_method_still_reached.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/nrc.c" >"$$tmp/nrc.out" 2>&1; then \
	  echo "reject-test: FAIL (a called class method's provable NoMethodError compiled)"; ok=0; \
	else grep -q "undefined method '\[\]=' for a Class" "$$tmp/nrc.out" || \
	  { echo "reject-test: FAIL (a called class method's NoMethodError rejected without saying why)"; sed -n 1,5p "$$tmp/nrc.out"; ok=0; }; fi; \
	t=test/reject/combination_safe_nav_two_params.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/csn.c" >"$$tmp/csn.out" 2>&1; then \
	  echo "reject-test: FAIL (a combination block taking two parameters through &. compiled)"; ok=0; \
	else grep -q "a block taking more than one parameter on combination" "$$tmp/csn.out" || \
	  { echo "reject-test: FAIL (a combination block through &. rejected without saying why)"; sed -n 1,5p "$$tmp/csn.out"; ok=0; }; fi; \
	t=test/reject/new_receiver_class_chain_still_reached.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/ncc.c" >"$$tmp/ncc.out" 2>&1; then \
	  echo "reject-test: FAIL (a called class-side super chain's provable NoMethodError compiled)"; ok=0; \
	else grep -q "undefined method '\[\]=' for a Class" "$$tmp/ncc.out" || \
	  { echo "reject-test: FAIL (a called class-side super chain's NoMethodError rejected without saying why)"; sed -n 1,5p "$$tmp/ncc.out"; ok=0; }; fi; \
	t=test/reject/redo_unlabeled_iterator.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/rui.c" >"$$tmp/rui.out" 2>&1; then \
	  echo "reject-test: FAIL (a redo with no label for it compiled)"; ok=0; \
	else grep -q "redo in this block" "$$tmp/rui.out" || \
	  { echo "reject-test: FAIL (a redo with no label rejected without saying why)"; sed -n 1,5p "$$tmp/rui.out"; ok=0; }; fi; \
	t=test/reject/instance_exec_default_ivar_write.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/idw.c" >"$$tmp/idw.out" 2>&1; then \
	  echo "reject-test: FAIL (an ivar written in a block default on a value with no ivars compiled)"; ok=0; \
	else grep -q "on a value with no instance variable layout" "$$tmp/idw.out" || \
	  { echo "reject-test: FAIL (an ivar written in a block default rejected without saying why)"; sed -n 1,5p "$$tmp/idw.out"; ok=0; }; fi; \
	t=test/reject/yield_splat_elem_append.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/yse.c" >"$$tmp/yse.out" 2>&1; then \
	  echo "reject-test: FAIL (an Array element splatted into a yield to an appending block compiled)"; ok=0; \
	else grep -q "from a value that is not a String variable" "$$tmp/yse.out" || \
	  { echo "reject-test: FAIL (an Array element splatted into a yield rejected without saying why)"; sed -n 1,5p "$$tmp/yse.out"; ok=0; }; fi; \
	t=test/reject/string_splat_changed_array.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/sca.c" >"$$tmp/sca.out" 2>&1; then \
	  echo "reject-test: FAIL (a global in a changed splatted Array compiled)"; ok=0; \
	else grep -q "through a splat of an Array the program changes" "$$tmp/sca.out" || \
	  { echo "reject-test: FAIL (changed splatted Array rejected without saying why)"; sed -n 1,5p "$$tmp/sca.out"; ok=0; }; fi; \
	t=test/reject/string_mutator_jump_arm.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/mja.c" >"$$tmp/mja.out" 2>&1; then \
	  echo "reject-test: FAIL (a String mutator on a conditional with a returning arm compiled)"; ok=0; \
	else grep -q "unsupported expression" "$$tmp/mja.out" || \
	  { echo "reject-test: FAIL (a returning arm under a String mutator rejected without saying why)"; sed -n 1,5p "$$tmp/mja.out"; ok=0; }; fi; \
	t=test/reject/string_global_handle_param.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/ghp.c" >"$$tmp/ghp.out" 2>&1; then \
	  echo "reject-test: FAIL (a global run first into a handle parameter compiled)"; ok=0; \
	else grep -q "parameter .io. through the call, which the method appends to" "$$tmp/ghp.out" || \
	  { echo "reject-test: FAIL (a global into a handle parameter rejected without saying why)"; sed -n 1,5p "$$tmp/ghp.out"; ok=0; }; fi; \
	t=test/reject/string_cvar_boxed_param.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/cbp.c" >"$$tmp/cbp.out" 2>&1; then \
	  echo "reject-test: FAIL (a class variable into a boxed parameter compiled)"; ok=0; \
	else grep -q "from a class variable into a parameter that boxes it" "$$tmp/cbp.out" || \
	  { echo "reject-test: FAIL (a class variable into a boxed parameter rejected without saying why)"; sed -n 1,5p "$$tmp/cbp.out"; ok=0; }; fi; \
	t=test/reject/string_splat_global_toplevel.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/sgt.c" >"$$tmp/sgt.out" 2>&1; then \
	  echo "reject-test: FAIL (a global splatted into a top-level method's appending parameter compiled)"; ok=0; \
	else grep -q "parameter .a. through a splat, which the method appends to" "$$tmp/sgt.out" || \
	  { echo "reject-test: FAIL (a global splatted into a top-level method rejected without saying why)"; sed -n 1,5p "$$tmp/sgt.out"; ok=0; }; fi; \
	t=test/reject/string_forward_rest_past16.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/frp.c" >"$$tmp/frp.out" 2>&1; then \
	  echo "reject-test: FAIL (a String forwarded past 16 positions compiled)"; ok=0; \
	else grep -q "through the rest it hands on" "$$tmp/frp.out" || \
	  { echo "reject-test: FAIL (a String forwarded past 16 positions rejected without saying why)"; sed -n 1,5p "$$tmp/frp.out"; ok=0; }; fi; \
	t=test/reject/string_yield_poly_param_global.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/ypg.c" >"$$tmp/ypg.out" 2>&1; then \
	  echo "reject-test: FAIL (a global yielded through a boxed parameter's alias into an appending block compiled)"; ok=0; \
	else grep -q "through a yield into a block argument" "$$tmp/ypg.out" || \
	  { echo "reject-test: FAIL (a global through a boxed parameter's alias rejected without saying why)"; sed -n 1,5p "$$tmp/ypg.out"; ok=0; }; fi; \
	t=test/reject/string_forward_poly_chain.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/fpc.c" >"$$tmp/fpc.out" 2>&1; then \
	  echo "reject-test: FAIL (a global through a POLY hand-on past the depth bound compiled)"; ok=0; \
	else grep -q "through a parameter it hands on" "$$tmp/fpc.out" || \
	  { echo "reject-test: FAIL (a global through a POLY hand-on past the depth bound rejected without saying why)"; sed -n 1,5p "$$tmp/fpc.out"; ok=0; }; fi; \
	for t in test/reject/string_kwsplat_last_dynamic.rb test/reject/string_kwsplat_last_yieldproc.rb test/reject/string_kwsplat_last_yieldblock.rb; do \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/kwlast.c" >"$$tmp/kwlast.out" 2>&1; then \
	  echo "reject-test: FAIL (a final splat carrying a String variable compiled: $$t)"; ok=0; \
	else grep -q 'splatted Hash literal' "$$tmp/kwlast.out" || \
	  { echo "reject-test: FAIL (a final splat rejected without saying why: $$t)"; sed -n 1,5p "$$tmp/kwlast.out"; ok=0; }; fi; \
	done; \
	t=test/reject/string_kwsplat_literal_dynamic.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/sk.c" >"$$tmp/sk.out" 2>&1; then \
	  echo "reject-test: FAIL (string_kwsplat_literal_dynamic compiled)"; ok=0; \
	else grep -q "through a splatted Hash literal" "$$tmp/sk.out" || \
	  { echo "reject-test: FAIL (string_kwsplat_literal_dynamic rejected without saying why)"; sed -n 1,5p "$$tmp/sk.out"; ok=0; }; fi; \
	t=test/reject/string_kwsplat_literal_yield.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/sk.c" >"$$tmp/sk.out" 2>&1; then \
	  echo "reject-test: FAIL (string_kwsplat_literal_yield compiled)"; ok=0; \
	else grep -q "through a splatted Hash literal" "$$tmp/sk.out" || \
	  { echo "reject-test: FAIL (string_kwsplat_literal_yield rejected without saying why)"; sed -n 1,5p "$$tmp/sk.out"; ok=0; }; fi; \
	t=test/reject/string_nested_masgn_target.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/sk.c" >"$$tmp/sk.out" 2>&1; then \
	  echo "reject-test: FAIL (string_nested_masgn_target compiled)"; ok=0; \
	else grep -q "through a nested multiple-assignment target" "$$tmp/sk.out" || \
	  { echo "reject-test: FAIL (string_nested_masgn_target rejected without saying why)"; sed -n 1,5p "$$tmp/sk.out"; ok=0; }; fi; \
	t=test/reject/string_nested_masgn_deep.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/sk.c" >"$$tmp/sk.out" 2>&1; then \
	  echo "reject-test: FAIL (string_nested_masgn_deep compiled)"; ok=0; \
	else grep -q "through a nested multiple-assignment target" "$$tmp/sk.out" || \
	  { echo "reject-test: FAIL (string_nested_masgn_deep rejected without saying why)"; sed -n 1,5p "$$tmp/sk.out"; ok=0; }; fi; \
	t=test/reject/string_ivar_alias_lent_call.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/sk.c" >"$$tmp/sk.out" 2>&1; then \
	  echo "reject-test: FAIL (string_ivar_alias_lent_call compiled)"; ok=0; \
	else grep -q "through a lent instance variable written from a local" "$$tmp/sk.out" || \
	  { echo "reject-test: FAIL (string_ivar_alias_lent_call rejected without saying why)"; sed -n 1,5p "$$tmp/sk.out"; ok=0; }; fi; \
	t=test/reject/string_ivar_alias_lent_super.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/sk.c" >"$$tmp/sk.out" 2>&1; then \
	  echo "reject-test: FAIL (string_ivar_alias_lent_super compiled)"; ok=0; \
	else grep -q "through a lent instance variable written from a local" "$$tmp/sk.out" || \
	  { echo "reject-test: FAIL (string_ivar_alias_lent_super rejected without saying why)"; sed -n 1,5p "$$tmp/sk.out"; ok=0; }; fi; \
	t=test/reject/string_duplicate_keyword_variable.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/sk.c" >"$$tmp/sk.out" 2>&1; then \
	  echo "reject-test: FAIL (string_duplicate_keyword_variable compiled)"; ok=0; \
	else grep -q "through a repeated keyword" "$$tmp/sk.out" || \
	  { echo "reject-test: FAIL (string_duplicate_keyword_variable rejected without saying why)"; sed -n 1,5p "$$tmp/sk.out"; ok=0; }; fi; \
	t=test/reject/string_rest_splat_yield.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/rsy.c" >"$$tmp/rsy.out" 2>&1; then \
	  echo "reject-test: FAIL (a String gathered into a rest yielded with a splat compiled)"; ok=0; \
	else grep -q "through a splat into a yield" "$$tmp/rsy.out" || \
	  { echo "reject-test: FAIL (a String gathered into a rest yielded with a splat rejected without saying why)"; sed -n 1,5p "$$tmp/rsy.out"; ok=0; }; fi; \
	t=test/reject/string_gather_short_splat_lead.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/gss.c" >"$$tmp/gss.out" 2>&1; then \
	  echo "reject-test: FAIL (a String a short splat can move onto another parameter compiled)"; ok=0; \
	else grep -q "ahead of a splat whose length decides which parameter takes it" "$$tmp/gss.out" || \
	  { echo "reject-test: FAIL (short-splat lead rejected without saying why)"; sed -n 1,5p "$$tmp/gss.out"; ok=0; }; fi; \
	t=test/reject/toplevel_include_yield_ivar_target.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/tiv.c" >"$$tmp/tiv.out" 2>&1; then \
	  echo "reject-test: FAIL (an included method assigning an ivar as a multiple-assignment target compiled)"; ok=0; \
	else grep -q "top-level include of a module method that uses instance variables" "$$tmp/tiv.out" || \
	  { echo "reject-test: FAIL (included ivar target rejected without saying why)"; sed -n 1,5p "$$tmp/tiv.out"; ok=0; }; fi; \
	t=test/reject/basicobject_toplevel_include_call.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/bo.c" >"$$tmp/bo.out" 2>&1; then \
	  echo "reject-test: FAIL (a BasicObject instance method reached a top-level included module method)"; ok=0; \
	else grep -q "unsupported call: node [0-9]* (CallNode \`hello\`)" "$$tmp/bo.out" || \
	  { echo "reject-test: FAIL (BasicObject bare call rejected without naming it)"; sed -n 1,5p "$$tmp/bo.out"; ok=0; }; fi; \
	for t in test/reject/super_init_value.rb test/reject/super_init_value_if.rb \
	         test/reject/super_init_value_begin.rb; do \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/siv.c" >"$$tmp/siv.out" 2>&1; then \
	  echo "reject-test: FAIL (the value of super in initialize compiled: $$t)"; ok=0; \
	else grep -q "unsupported value of \`super\` in initialize" "$$tmp/siv.out" || \
	  { echo "reject-test: FAIL (the value of super in initialize rejected without saying why: $$t)"; sed -n 1,5p "$$tmp/siv.out"; ok=0; }; fi; \
	done; \
	t=test/reject/forwarding_super_yielding_optional.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/fy.c" >"$$tmp/fy.out" 2>&1; then \
	  echo "reject-test: FAIL (super(...) into a yielding parent with an optional compiled)"; ok=0; \
	else grep -q "the forwarded arguments cannot leave one out" "$$tmp/fy.out" || \
	  { echo "reject-test: FAIL (super(...) into a yielding parent rejected without saying why)"; sed -n 1,5p "$$tmp/fy.out"; ok=0; }; fi; \
	t=test/reject/forwarding_builtin_uneven_calls.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/fb.c" >"$$tmp/fb.out" 2>&1; then \
	  echo "reject-test: FAIL (... into a builtin from calls of different arities compiled)"; ok=0; \
	else grep -q "forwarded into a builtin method, from calls that do not all pass the same number" "$$tmp/fb.out" || \
	  { echo "reject-test: FAIL (... into a builtin rejected without saying why)"; sed -n 1,5p "$$tmp/fb.out"; ok=0; }; fi; \
	t=test/reject/class_then_module.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/m.c" >"$$tmp/m.out" 2>&1; then \
	  echo "reject-test: FAIL (#4309: a constant declared class and then module compiled)"; ok=0; \
	else grep -q "Thing is not a module" "$$tmp/m.out" || \
	  { echo "reject-test: FAIL (#4309: rejected without saying why)"; sed -n 1,5p "$$tmp/m.out"; ok=0; }; fi; \
	t=test/reject/superclass_mismatch.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/s.c" >"$$tmp/s.out" 2>&1; then \
	  echo "reject-test: FAIL (#4309: a class reopened with another superclass compiled)"; ok=0; \
	else grep -q "superclass mismatch for class Thing" "$$tmp/s.out" || \
	  { echo "reject-test: FAIL (#4309: rejected without saying why)"; sed -n 1,5p "$$tmp/s.out"; ok=0; }; fi; \
	for t in const_value_not_a_class const_value_not_a_module; do \
	  if $(SPINEL) "test/reject/$$t.rb" -c --no-line-map -o "$$tmp/$$t.c" >"$$tmp/$$t.out" 2>&1; then \
	    echo "reject-test: FAIL (#4318: $$t compiled)"; ok=0; \
	  else grep -Eq "is not a (class|module)" "$$tmp/$$t.out" || \
	    { echo "reject-test: FAIL (#4318: $$t rejected without saying why)"; sed -n 1,3p "$$tmp/$$t.out"; ok=0; }; fi; \
	done; \
	t=test/reject/typed_array_kept_by_struct_into_mutated_param.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/tp.c" >"$$tmp/tp.out" 2>&1; then \
	  echo "reject-test: FAIL (#4480: a typed array copied into a mutated general-Array parameter compiled)"; ok=0; \
	else grep -q "which the method mutates" "$$tmp/tp.out" || \
	  { echo "reject-test: FAIL (#4480: rejected without saying why)"; sed -n 1,5p "$$tmp/tp.out"; ok=0; }; fi; \
	t=test/reject/string_append_through_new_global.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/sn.c" >"$$tmp/sn.out" 2>&1; then \
	  echo "reject-test: FAIL (#6179: a global's String copied into an appending initialize compiled)"; ok=0; \
	else grep -q "which the method appends to" "$$tmp/sn.out" || \
	  { echo "reject-test: FAIL (#6179: rejected without saying why)"; sed -n 1,5p "$$tmp/sn.out"; ok=0; }; fi; \
	t=test/reject/string_append_through_proc_keyword.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/sk.c" >"$$tmp/sk.out" 2>&1; then \
	  echo "reject-test: FAIL (#6179: a block parameter's String copied into a proc's appending keyword compiled)"; ok=0; \
	else grep -q "which the proc appends to" "$$tmp/sk.out" || \
	  { echo "reject-test: FAIL (#6179: keyword rejected without saying why)"; sed -n 1,5p "$$tmp/sk.out"; ok=0; }; fi; \
	t=test/reject/string_append_through_kept_block_index.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/kb.c" >"$$tmp/kb.out" 2>&1; then \
	  echo "reject-test: FAIL (#6179: a block parameter's String copied through a kept block's b[] compiled)"; ok=0; \
	else grep -q "a proc or Method it can reach appends to the String" "$$tmp/kb.out" || \
	  { echo "reject-test: FAIL (#6179: b[] rejected without saying why)"; sed -n 1,5p "$$tmp/kb.out"; ok=0; }; fi; \
	for t in string_lent_global_rebound string_lent_ivar_rebound string_lent_global_rebound_block; do \
	  if $(SPINEL) "test/reject/$$t.rb" -c --no-line-map -o "$$tmp/$$t.c" >"$$tmp/$$t.out" 2>&1; then \
	    echo "reject-test: FAIL (#6179: $$t, a lent global slot assigned during the call, compiled)"; ok=0; \
	  else grep -q "where the assignment can run during the call" "$$tmp/$$t.out" || \
	    { echo "reject-test: FAIL (#6179: $$t rejected without saying why)"; sed -n 1,5p "$$tmp/$$t.out"; ok=0; }; fi; \
	done; \
	t=test/reject/typed_array_kept_by_struct_into_boxed_param_store.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/tb.c" >"$$tmp/tb.out" 2>&1; then \
	  echo "reject-test: FAIL (a typed array held by the caller, stored into through a boxed parameter, compiled)"; ok=0; \
	else grep -q "which the method stores elements of other kinds into" "$$tmp/tb.out" || \
	  { echo "reject-test: FAIL (a held typed array into a boxed parameter's store rejected without saying why)"; sed -n 1,5p "$$tmp/tb.out"; ok=0; }; fi; \
	t=test/reject/const_get_runtime_name.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/cg.c" >"$$tmp/cg.out" 2>&1; then \
	  echo "reject-test: FAIL (a const_get with a run-time name on a class value compiled)"; ok=0; \
	else grep -q "const_get with a name known only at run time" "$$tmp/cg.out" || \
	  { echo "reject-test: FAIL (a run-time const_get refused without saying why)"; sed -n 1,5p "$$tmp/cg.out"; ok=0; }; fi; \
	for t in test/reject/macro_*.rb; do \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/mc.c" >"$$tmp/mc.out" 2>&1; then \
	  echo "reject-test: FAIL ($$t: a macro over state or a name it cannot compute was expanded)"; ok=0; \
	else grep -qE "unsupported send with a runtime method name|unsupported call: .*CallNode .(module_eval|const_set).|no class in the program defines" "$$tmp/mc.out" || \
	  { echo "reject-test: FAIL ($$t: refused without saying why)"; sed -n 1,5p "$$tmp/mc.out"; ok=0; }; fi; \
	done; \
	for t in test/reject/class_eval_static_*.rb; do \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/ce.c" >"$$tmp/ce.out" 2>&1; then \
	  echo "reject-test: FAIL ($$t: a class_eval string that reads differently spliced was grafted)"; ok=0; \
	else grep -qE "unsupported call: .*CallNode .class_eval." "$$tmp/ce.out" || \
	  { echo "reject-test: FAIL ($$t: refused without saying why)"; sed -n 1,5p "$$tmp/ce.out"; ok=0; }; fi; \
	done; \
	t=test/reject/def_delegators_splat.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/dd.c" >"$$tmp/dd.out" 2>&1; then \
	  echo "reject-test: FAIL (a def_delegators the parser could not rewrite compiled)"; ok=0; \
	else grep -q "def_delegators with arguments other than a literal symbol list" "$$tmp/dd.out" || \
	  { echo "reject-test: FAIL (a def_delegators refused without saying why)"; sed -n 1,5p "$$tmp/dd.out"; ok=0; }; fi; \
	t=test/reject/recursive_default_reads_block.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/rd.c" >"$$tmp/rd.out" 2>&1; then \
	  echo "reject-test: FAIL (a recursive default reading the block compiled)"; ok=0; \
	else grep -q "calls \`m\` again with that argument omitted" "$$tmp/rd.out" || \
	  { echo "reject-test: FAIL (a recursive default refused without saying why)"; sed -n 1,5p "$$tmp/rd.out"; ok=0; }; fi; \
	t=test/reject/ffi_ptr_ruby_object.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/fp.c" >"$$tmp/fp.out" 2>&1; then \
	  echo "reject-test: FAIL (a Ruby object passed to an ffi pointer slot compiled)"; ok=0; \
	else grep -q "a Ruby object has no C address" "$$tmp/fp.out" || \
	  { echo "reject-test: FAIL (an ffi pointer slot refused a Ruby object without saying why)"; sed -n 1,5p "$$tmp/fp.out"; ok=0; }; fi; \
	t=test/reject/class_eval_on_class_in_method.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/ce.c" >"$$tmp/ce.out" 2>&1; then \
	  echo "reject-test: FAIL (a Class.class_eval inside a method compiled)"; ok=0; \
	else grep -q "methods are added to Class only by a top-level" "$$tmp/ce.out" || \
	  { echo "reject-test: FAIL (a Class.class_eval inside a method refused without saying why)"; sed -n 1,5p "$$tmp/ce.out"; ok=0; }; fi; \
	t=test/reject/class_eval_on_class_with_params.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/cp.c" >"$$tmp/cp.out" 2>&1; then \
	  echo "reject-test: FAIL (a Class.class_eval with block parameters compiled)"; ok=0; \
	else grep -q "only with a literal block and no block parameters or arguments" "$$tmp/cp.out" || \
	  { echo "reject-test: FAIL (a Class.class_eval with block parameters refused without saying why)"; sed -n 1,5p "$$tmp/cp.out"; ok=0; }; fi; \
	t=test/reject/class_reopen_reserved_name.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/cr.c" >"$$tmp/cr.out" 2>&1; then \
	  echo "reject-test: FAIL (a program declaring Class__reopen compiled)"; ok=0; \
	else grep -q "Class__reopen is reserved" "$$tmp/cr.out" || \
	  { echo "reject-test: FAIL (a program declaring Class__reopen refused without saying why)"; sed -n 1,5p "$$tmp/cr.out"; ok=0; }; fi; \
	t=test/reject/instance_exec_refusal_moved_scope.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/ie.c" >"$$tmp/ie.out" 2>&1; then \
	  echo "reject-test: FAIL (ObjectSpace in an instance_exec block compiled)"; ok=0; \
	else grep -q "1 refusal," "$$tmp/ie.out" || \
	  { echo "reject-test: FAIL (a refusal in an instance_exec block left its method moved, and a later call was refused too)"; sed -n 1,5p "$$tmp/ie.out"; ok=0; }; fi; \
	t=test/reject/method_of_builtin_module.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/mb.c" >"$$tmp/mb.out" 2>&1; then \
	  echo "reject-test: FAIL (a Method of a builtin module function compiled)"; ok=0; \
	else grep -q "ENV.method(:each) is not supported: a Method object of a builtin module" "$$tmp/mb.out" || \
	  { echo "reject-test: FAIL (a Method of a builtin module function refused without saying why)"; sed -n 1,5p "$$tmp/mb.out"; ok=0; }; fi; \
	for t in test/reject/compare_by_identity_chained*.rb; do \
	  if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/cbi.c" >"$$tmp/cbi.out" 2>&1; then \
	    echo "reject-test: FAIL ($$t: a call chained onto compare_by_identity compiled)"; ok=0; \
	  else grep -q "unsupported Hash#compare_by_identity" "$$tmp/cbi.out" || \
	    { echo "reject-test: FAIL ($$t: refused without saying why)"; sed -n 1,5p "$$tmp/cbi.out"; ok=0; }; fi; \
	done; \
	t=test/reject/bare_const_nested_unreachable.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/bcn.c" >"$$tmp/bcn.out" 2>&1; then \
	  echo "reject-test: FAIL (a bare constant CRuby's lookup cannot reach compiled, bound to a nested one)"; ok=0; \
	else grep -q "uninitialized constant X (NameError): the program defines it only as A::X" "$$tmp/bcn.out" || \
	  { echo "reject-test: FAIL (an unreachable bare constant refused without saying why)"; sed -n 1,5p "$$tmp/bcn.out"; ok=0; }; fi; \
	t=test/reject/bare_const_compact_class_path.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/bcp.c" >"$$tmp/bcp.out" 2>&1; then \
	  echo "reject-test: FAIL (a constant of A read bare from a class A::B body compiled)"; ok=0; \
	else grep -q "uninitialized constant A::B::LIMIT (NameError)" "$$tmp/bcp.out" || \
	  { echo "reject-test: FAIL (a constant of A read bare from class A::B refused without saying why)"; sed -n 1,5p "$$tmp/bcp.out"; ok=0; }; fi; \
	for t in test/reject/systemcallerror_subclass_errno_const*.rb; do \
	  if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/sce.c" >"$$tmp/sce.out" 2>&1; then \
	    echo "reject-test: FAIL ($$t: an Errno constant in a subclass of SystemCallError compiled)"; ok=0; \
	  else grep -q "an Errno constant defined in a subclass of SystemCallError" "$$tmp/sce.out" || \
	    { echo "reject-test: FAIL ($$t: refused without saying why)"; sed -n 1,5p "$$tmp/sce.out"; ok=0; }; fi; \
	done; \
	t=test/reject/systemcallerror_zsuper_keyword.rb; \
	if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/szk.c" >"$$tmp/szk.out" 2>&1; then \
	  echo "reject-test: FAIL (a bare super forwarding a keyword to SystemCallError#initialize compiled)"; ok=0; \
	else grep -q "a bare super forwarding a rest, keyword or block parameter to SystemCallError#initialize" "$$tmp/szk.out" || \
	  { echo "reject-test: FAIL (a bare super forwarding a keyword refused without saying why)"; sed -n 1,5p "$$tmp/szk.out"; ok=0; }; fi; \
	for t in test/reject/object_receiver_include*.rb; do \
	  if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/ori.c" >"$$tmp/ori.out" 2>&1; then \
	    echo "reject-test: FAIL ($$t: an include into Object through an explicit receiver compiled)"; ok=0; \
	  else grep -q "Object.include(...) is not supported by AOT compilation" "$$tmp/ori.out" || \
	    { echo "reject-test: FAIL ($$t: refused without saying why)"; sed -n 1,5p "$$tmp/ori.out"; ok=0; }; fi; \
	done; \
	t=test/reject/dynamic_send_then_refusal.rb; \
	$(SPINEL) "$$t" -c --no-line-map -o "$$tmp/ds.c" >"$$tmp/ds.out" 2>&1; st=$$?; \
	if [ $$st -ne 1 ] || ! grep -q "1 refusal," "$$tmp/ds.out"; then \
	  echo "reject-test: FAIL (a refusal after a dynamic send's probed arms did not report cleanly, exit $$st)"; sed -n 1,5p "$$tmp/ds.out"; ok=0; fi; \
	for spec in "complex_bignum_component:a Complex component given an Integer past 64 bits" \
	            "rational_pow_bignum:the receiver of a Float \`**\` given a Rational" \
	            "bignum_div_rational:an Integer operand of a Bignum operation given a Rational" \
	            "array_push_other_class_temporary:an Array push given a String"; do \
	  t=test/reject/$${spec%%:*}.rb; why=$${spec#*:}; \
	  if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/co.c" >"$$tmp/co.out" 2>&1; then \
	    echo "reject-test: FAIL ($$t compiled: a value no conversion keeps went into its slot)"; ok=0; \
	  else grep -qF "$$why" "$$tmp/co.out" || \
	    { echo "reject-test: FAIL ($$t refused without saying why)"; sed -n 1,5p "$$tmp/co.out"; ok=0; }; fi; \
	done; \
	for spec in "subclass_array:class Stack < Array: subclassing Array is not supported yet" \
	            "subclass_hash:class Registry < Hash: subclassing Hash is not supported yet" \
	            "subclass_string:class Name < String: subclassing String is not supported yet" \
	            "subclass_hash_own_methods_only:class Opts < Hash: subclassing Hash" \
	            "subclass_array_toplevel_path:class Points < Array: subclassing Array" \
	            "subclass_hash_class_new:Class.new(Hash): subclassing Hash" \
	            "subclass_hash_class_new_block:class Registry < Hash: subclassing Hash" \
	            "subclass_range:class Span < Range: subclassing Range" \
	            "subclass_thread_queue:class Jobs < Queue: subclassing Queue" \
	            "subclass_stringio:class Buffer < StringIO: subclassing StringIO"; do \
	  t=test/reject/$${spec%%:*}.rb; why=$${spec#*:}; \
	  if $(SPINEL) "$$t" -c --no-line-map -o "$$tmp/sb.c" >"$$tmp/sb.out" 2>&1; then \
	    echo "reject-test: FAIL ($$t compiled: a subclass of a builtin has none of its parent's methods)"; ok=0; \
	  else grep -qF "$$why" "$$tmp/sb.out" || \
	    { echo "reject-test: FAIL ($$t refused without saying why)"; sed -n 1,5p "$$tmp/sb.out"; ok=0; }; fi; \
	done; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "reject-test: pass"; else exit 1; fi

# ---- Minor-mark leg (#4311) ----
# The generational mark is the default (2026-09-12), so the suite runs under
# it; this leg keeps the other mode honest too. Each program here is built to
# catch a missed write barrier -- a young value stored into a long-lived
# thread-local map, held only by the map between the write and the read --
# and runs with the mark on and off, then under the verifier with stress.
# It costs a fifth of a second, and it is the leg that was missing when a
# barrier pointed at the wrong object shipped.
# One leg per barrier gap that shipped. A single program was what this target
# ran when a store into a capture cell shipped with no barrier at all, so a
# fix here adds its reproducer to the list rather than testing by hand.
# SPINEL_GC_PHASES only reports; it must not change what a program computes, and
# it must say nothing at all when it is off. Both halves are the contract, and
# neither is visible to the ordinary harness, which cannot vary the environment
# per test.
gc-phases-test: $(SPINEL) $(SP_RT_LIB) $(SP_RT_MT_LIB) $(SPINEL_TIMEOUT)
	@tmp=$$(mktemp -d /tmp/spinel-gcph.XXXXXX); ok=1; \
	src=test/gc_minor_thread_local_slot.rb; \
	$(SPINEL) "$$src" -o "$$tmp/m" >/dev/null 2>&1 || \
	  { echo "gc-phases-test: FAIL (compile)"; rm -rf "$$tmp"; exit 1; }; \
	$(TIMEOUT60) "$$tmp/m" > "$$tmp/off.out" 2> "$$tmp/off.err"; \
	SPINEL_GC_PHASES=1 $(TIMEOUT60) "$$tmp/m" > "$$tmp/on.out" 2> "$$tmp/on.err"; \
	if ! cmp -s "$$tmp/off.out" "$$tmp/on.out"; then \
	  echo "gc-phases-test: FAIL (stdout differs with the flag set)"; \
	  diff -u "$$tmp/off.out" "$$tmp/on.out" | head -10; ok=0; fi; \
	if [ -s "$$tmp/off.err" ]; then \
	  echo "gc-phases-test: FAIL (wrote to stderr with the flag unset)"; \
	  head -3 "$$tmp/off.err"; ok=0; fi; \
	if ! grep -q '^\[gcph\] mark ' "$$tmp/on.err"; then \
	  echo "gc-phases-test: FAIL (no [gcph] phase line with the flag set)"; ok=0; fi; \
	if ! grep -q '^\[gcph\] mark: ' "$$tmp/on.err"; then \
	  echo "gc-phases-test: FAIL (no [gcph] mark split with the flag set)"; ok=0; fi; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "gc-phases-test: pass"; else exit 1; fi

# The two per-heap collection floors move ONE trigger each, which is the whole
# point of having them: moving both together cannot say which heap paces the
# collections (#4384). Read back off SPINEL_GC_STATS, which reports the two
# separately, so the test asserts the mechanism rather than a timing.
gc-threshold-test: $(SPINEL) $(SP_RT_LIB) $(SP_RT_MT_LIB) $(SPINEL_TIMEOUT)
	@tmp=$$(mktemp -d /tmp/spinel-gcthr.XXXXXX); ok=1; \
	src=test/gc_threshold_per_heap.rb; \
	$(SPINEL) "$$src" -o "$$tmp/t" >/dev/null 2>&1 || \
	  { echo "gc-threshold-test: FAIL (compile)"; rm -rf "$$tmp"; exit 1; }; \
	SPINEL_GC_STATS=1 $(TIMEOUT60) "$$tmp/t" >/dev/null 2> "$$tmp/base.err"; \
	SPINEL_GC_THRESHOLD_OBJ_KB=16384 SPINEL_GC_STATS=1 $(TIMEOUT60) "$$tmp/t" >/dev/null 2> "$$tmp/obj.err"; \
	SPINEL_GC_THRESHOLD_STR_KB=16384 SPINEL_GC_STATS=1 $(TIMEOUT60) "$$tmp/t" >/dev/null 2> "$$tmp/str.err"; \
	first_obj() { sed -n 's/.*trigger \([0-9.]*\) MB obj.*/\1/p' "$$1" | head -1; }; \
	first_str() { sed -n 's/.*+ \([0-9.]*\) MB str.*/\1/p' "$$1" | head -1; }; \
	bo=$$(first_obj "$$tmp/base.err"); bs=$$(first_str "$$tmp/base.err"); \
	oo=$$(first_obj "$$tmp/obj.err");  os=$$(first_str "$$tmp/obj.err"); \
	so=$$(first_obj "$$tmp/str.err");  ss=$$(first_str "$$tmp/str.err"); \
	for v in "$$bo" "$$bs" "$$oo" "$$os" "$$so" "$$ss"; do \
	  [ -n "$$v" ] || { echo "gc-threshold-test: FAIL (no trigger line from SPINEL_GC_STATS)"; ok=0; break; }; \
	done; \
	awk -v a="$$oo" -v b="$$bo" 'BEGIN{exit !(a > b * 8)}' || \
	  { echo "gc-threshold-test: FAIL (OBJ_KB did not raise the object trigger: $$bo -> $$oo)"; ok=0; }; \
	awk -v a="$$os" -v b="$$bs" 'BEGIN{exit !(a < b * 4)}' || \
	  { echo "gc-threshold-test: FAIL (OBJ_KB moved the string trigger too: $$bs -> $$os)"; ok=0; }; \
	awk -v a="$$ss" -v b="$$bs" 'BEGIN{exit !(a > b * 8)}' || \
	  { echo "gc-threshold-test: FAIL (STR_KB did not raise the string trigger: $$bs -> $$ss)"; ok=0; }; \
	awk -v a="$$so" -v b="$$bo" 'BEGIN{exit !(a < b * 4)}' || \
	  { echo "gc-threshold-test: FAIL (STR_KB moved the object trigger too: $$bo -> $$so)"; ok=0; }; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "gc-threshold-test: pass"; else exit 1; fi

# The string major has two policies and this leg asserts the difference is the
# one they claim, on a shape where the SIZE gate ratchets: a 12 MB live string
# set under a 4 MB pinned floor, so promotion runs ahead of the gate. The
# schedule is the default and SPINEL_GC_STR_MAJOR=size is the way back, so the
# leg also pins which of the two ships. Everything here is a comparison of the
# two arms in the SAME run of the same binary on the same machine -- no absolute
# either arm has to hit, because both numbers are set by how much this box
# promotes (#4407).
gc-str-major-test: $(SPINEL) $(SP_RT_LIB) $(SP_RT_MT_LIB) $(SPINEL_TIMEOUT)
	@tmp=$$(mktemp -d /tmp/spinel-gcstrmaj.XXXXXX); ok=1; \
	src=test/gc_str_major_interval.rb; \
	$(SPINEL) "$$src" -o "$$tmp/t" >/dev/null 2>&1 || \
	  { echo "gc-str-major-test: FAIL (compile)"; rm -rf "$$tmp"; exit 1; }; \
	run() { SPINEL_GC_STR_BUDGET=fixed SPINEL_GC_THRESHOLD_STR_KB=4096 \
	        SPINEL_GC_PHASES=1 $(TIMEOUT60) "$$tmp/t" > "$$tmp/$$1.out" 2> "$$tmp/$$1.err"; }; \
	run default; \
	SPINEL_GC_STR_MAJOR=size; export SPINEL_GC_STR_MAJOR; run size; \
	unset SPINEL_GC_STR_MAJOR; \
	for m in default size; do \
	  cmp -s "$$tmp/$$m.out" "$$src.expected" || \
	    { echo "gc-str-major-test: FAIL ($$m changed the answer)"; ok=0; }; \
	done; \
	old_of() { sed -n 's/.*+ \([0-9.]*\) MB old .*/\1/p' "$$1" | tail -1; }; \
	maj_of() { sed -n 's/.*old, \([0-9]*\) so far.*/\1/p' "$$1" | tail -1; }; \
	do=$$(old_of "$$tmp/default.err"); io=$$(old_of "$$tmp/size.err"); \
	dm=$$(maj_of "$$tmp/default.err"); im=$$(maj_of "$$tmp/size.err"); \
	for v in "$$do" "$$io" "$$dm" "$$im"; do \
	  [ -n "$$v" ] || { echo "gc-str-major-test: FAIL (no [gcph] string live line)"; ok=0; break; }; \
	done; \
	grep -q "major every .* sweeps, backstop " "$$tmp/default.err" || \
	  { echo "gc-str-major-test: FAIL (the schedule is not the default)"; ok=0; }; \
	grep -q "major at " "$$tmp/size.err" || \
	  { echo "gc-str-major-test: FAIL (=size did not restore the size gate)"; ok=0; }; \
	awk -v a="$$dm" -v b="$$im" 'BEGIN{exit !(a > b)}' || \
	  { echo "gc-str-major-test: FAIL (the schedule did not run more majors: $$im -> $$dm)"; ok=0; }; \
	awk -v a="$$do" -v b="$$io" 'BEGIN{exit !(a <= b)}' || \
	  { echo "gc-str-major-test: FAIL (the schedule left MORE old behind: $$io -> $$do)"; ok=0; }; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "gc-str-major-test: pass"; else exit 1; fi

# The object budget is priced off objects PLUS strings by default;
# SPINEL_GC_OBJ_BUDGET=obj restores pricing it off the object heap alone.
# The assertion is a WITHIN-RUN invariant -- the trigger against the live set
# that same line reports -- not a comparison of one run's number with another's.
# Both triggers retune continuously, so two runs' last lines are two different
# moments, which is what made a cross-run version of this flake.
gc-obj-budget-test: $(SPINEL) $(SP_RT_LIB) $(SPINEL_TIMEOUT)
	@tmp=$$(mktemp -d /tmp/spinel-gcobj.XXXXXX); ok=1; \
	str_of() { sed -n 's/.*+ \([0-9.]*\) MB str; trigger.*/\1/p' "$$1" | tail -1; }; \
	trg_of() { sed -n 's/.*trigger \([0-9.]*\) MB obj.*/\1/p' "$$1" | tail -1; }; \
	for prog in gc_obj_budget_walk gc_obj_budget_mark; do \
	  $(SPINEL) test/$$prog.rb -o "$$tmp/$$prog" >/dev/null 2>&1 || \
	    { echo "gc-obj-budget-test: FAIL ($$prog: compile)"; ok=0; continue; }; \
	  for mode in default walk obj; do \
	    if [ "$$mode" = default ]; then unset SPINEL_GC_OBJ_BUDGET; \
	    else SPINEL_GC_OBJ_BUDGET=$$mode; export SPINEL_GC_OBJ_BUDGET; fi; \
	    SPINEL_GC_STATS=1 $(TIMEOUT60) "$$tmp/$$prog" > "$$tmp/$$prog.$$mode.out" 2> "$$tmp/$$prog.$$mode.err"; \
	    cmp -s "$$tmp/$$prog.$$mode.out" test/$$prog.rb.expected || \
	      { echo "gc-obj-budget-test: FAIL ($$prog: $$mode changed the answer)"; ok=0; }; \
	  done; \
	  unset SPINEL_GC_OBJ_BUDGET; \
	done; \
	ws=$$(str_of "$$tmp/gc_obj_budget_walk.obj.err"); \
	ot=$$(trg_of "$$tmp/gc_obj_budget_walk.obj.err"); \
	wt=$$(trg_of "$$tmp/gc_obj_budget_walk.walk.err"); \
	gt=$$(trg_of "$$tmp/gc_obj_budget_walk.default.err"); \
	mo=$$(trg_of "$$tmp/gc_obj_budget_mark.obj.err"); \
	mw=$$(trg_of "$$tmp/gc_obj_budget_mark.walk.err"); \
	mg=$$(trg_of "$$tmp/gc_obj_budget_mark.default.err"); \
	[ -n "$$ws" ] && [ -n "$$wt" ] && [ -n "$$gt" ] && [ -n "$$mg" ] || \
	  { echo "gc-obj-budget-test: FAIL (no trigger line)"; ok=0; }; \
	awk -v t="$$ot" -v s="$$ws" 'BEGIN{exit !(t < s)}' || \
	  { echo "gc-obj-budget-test: FAIL (obj still saw the strings: trigger $$ot vs live str $$ws)"; ok=0; }; \
	awk -v t="$$wt" -v s="$$ws" 'BEGIN{exit !(t > s)}' || \
	  { echo "gc-obj-budget-test: FAIL (walk did not price in the strings: trigger $$wt vs live str $$ws)"; ok=0; }; \
	awk -v g="$$gt" -v w="$$wt" 'BEGIN{exit !(g < w)}' || \
	  { echo "gc-obj-budget-test: FAIL (the gate widened a sweep-bound program: $$gt vs walk $$wt)"; ok=0; }; \
	awk -v g="$$mg" -v o="$$mo" 'BEGIN{exit !(g > o)}' || \
	  { echo "gc-obj-budget-test: FAIL (the gate did not widen a mark-bound program: $$mg vs obj $$mo)"; ok=0; }; \
	awk -v g="$$mg" -v w="$$mw" 'BEGIN{exit !(g > 0.99 * w)}' || \
	  { echo "gc-obj-budget-test: FAIL (the gate fell short of walk on a mark-bound program: $$mg vs walk $$mw)"; ok=0; }; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "gc-obj-budget-test: pass"; else exit 1; fi

# A byref String parameter that a lifted proc also captures: the capture holds
# the CALLER's slot, which for the stack shape is not a GC object, and marking
# it read a header off the stack (#4391). Run under GC stress because the fault
# needs a collection while the proc is live -- with stress that is every
# allocation, which makes it deterministic; without it the program is quiet.
# ---- puts from several threads lands whole lines ----
# The text and the newline used to be two stdio calls, and another worker's
# puts could land between them. The interleaving of whole lines is free to
# vary, so the check is by shape: every line is W/I and there are 8 x 300.
thread-puts-test: $(SPINEL) $(SP_RT_LIB) $(SPINEL_TIMEOUT)
	@tmp=$$(mktemp -d /tmp/spinel-tputs.XXXXXX); ok=1; \
	$(SPINEL) test/threads/puts_lines_atomic.rb -o "$$tmp/p" >/dev/null 2>&1 || \
	  { echo "thread-puts-test: FAIL (compile)"; rm -rf "$$tmp"; exit 1; }; \
	for r in 1 2 3; do \
	  $(TIMEOUT60) "$$tmp/p" > "$$tmp/out" 2>/dev/null || { echo "thread-puts-test: FAIL (crashed or timed out)"; ok=0; }; \
	  n=$$(wc -l < "$$tmp/out"); bad=$$(grep -vcE '^[0-7]/[0-9]+$$' "$$tmp/out"); \
	  [ "$$n" -eq 2400 ] && [ "$$bad" -eq 0 ] || { echo "thread-puts-test: FAIL (run $$r: $$n lines, $$bad malformed)"; grep -vE '^[0-7]/[0-9]+$$' "$$tmp/out" | head -3; ok=0; }; \
	done; \
	$(SPINEL) test/threads/ffi_blocking_under_gc.rb -o "$$tmp/f" >/dev/null 2>&1 || \
	  { echo "thread-puts-test: FAIL (ffi blocking: compile)"; rm -rf "$$tmp"; exit 1; }; \
	for r in 1 2; do \
	  SPINEL_GC_STRESS=1 $(TIMEOUT60) "$$tmp/f" > "$$tmp/fout" 2>/dev/null || { echo "thread-puts-test: FAIL (ffi blocking: crashed or timed out under GC stress)"; ok=0; }; \
	  grep -q '^\[177, 177, 177, 177, 177, 177\]$$' "$$tmp/fout" || { echo "thread-puts-test: FAIL (ffi blocking: a value held across the call was lost)"; head -3 "$$tmp/fout"; ok=0; }; \
	done; \
	$(SPINEL) test/threads/fiber_stack_overflow.rb -o "$$tmp/s" >/dev/null 2>&1 || \
	  { echo "thread-puts-test: FAIL (fiber stack overflow: compile)"; rm -rf "$$tmp"; exit 1; }; \
	$(TIMEOUT60) "$$tmp/s" > "$$tmp/sout" 2> "$$tmp/serr"; rc=$$?; \
	[ $$rc -ne 0 ] && [ $$rc -ne 124 ] || { echo "thread-puts-test: FAIL (fiber stack overflow: the program did not die, rc=$$rc)"; ok=0; }; \
	grep -q 'fiber stack overflow' "$$tmp/serr" || { echo "thread-puts-test: FAIL (fiber stack overflow: the fault was not reported as one)"; head -3 "$$tmp/serr"; ok=0; }; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "thread-puts-test: pass"; else exit 1; fi

byref-capture-test: $(SPINEL) $(RBS_EXTRACT_BIN) $(SP_RT_LIB) $(SPINEL_TIMEOUT)
	@tmp=$$(mktemp -d /tmp/spinel-byrefcap.XXXXXX); ok=1; \
	$(SPINEL) test/rbs-seed/byref_capture_scan.rb --rbs test/rbs-seed/sig -o "$$tmp/b" >/dev/null 2>&1 || \
	  { echo "byref-capture-test: FAIL (compile)"; rm -rf "$$tmp"; exit 1; }; \
	SPINEL_GC_STRESS=1 $(TIMEOUT60) "$$tmp/b" > "$$tmp/out" 2>/dev/null || \
	  { echo "byref-capture-test: FAIL (crashed or timed out under GC stress)"; ok=0; }; \
	cmp -s "$$tmp/out" test/rbs-seed/byref_capture_scan.expected || \
	  { echo "byref-capture-test: FAIL (output differs)"; diff -u test/rbs-seed/byref_capture_scan.expected "$$tmp/out" | head -5; ok=0; }; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "byref-capture-test: pass"; else exit 1; fi

# ---- Allocation locality: the same graph, laid down by 1 worker or by N ----
# test/gc_locality_build.rb answers what the mark split in #4384 left open. The
# root walk is 0.000 s at every worker count, so the mark's rise with workers
# is the trace; this separates "who allocated the graph" from "who marks it" by
# holding the second still and varying the first. Measured on the ladder in its
# header, the per-object trace cost is set by the number of workers that
# ALLOCATED -- 3.74 ns at one, 5.56 at two, and flat out to sixteen.
#
# What is gated here is the correctness half of that, which is worth having on
# its own: a graph built across N workers must BE the graph built by one. The
# checksum is over the whole structure, so a node allocated on one worker and
# published to another without the store being seen -- or a slot recycled while
# still reachable -- changes it. That is the shape of three of the defects this
# workload family has found, and none of them crashed.
#
# The collection floor is here for the same reason as in threaded-render-test:
# an arm that stops collecting still prints the right checksum and defends
# nothing. The marks-per-collection floor beside it asks whether the graph is
# held live, which is a question about the FULL mark: that probe runs with
# SPINEL_GC_MINOR=0, since a minor cycle walks the young objects plus the
# remembered set and would answer a much smaller number for the same graph.
#
# LOCALITY_CHURN is NOT varied per arm, and that is deliberate twice over. It
# is what decides how many workers a cell really runs -- the pool grows toward
# one worker per live green thread, so SPINEL_WORKERS is a cap and the churn
# count is the demand -- and the program's default of 8 is what makes the W=8
# arm below an eight-worker run rather than a six-worker one. Holding it equal
# across the arms is also what lets every arm be compared against ONE expected
# file: it is part of the answer, so varying it per cell would mean either a
# file per cell or dropping it from the comparison, and it is the wrong thing
# to drop.
GC_LOCALITY_SRC := test/gc_locality_build.rb

gc-locality-test: $(SPINEL) $(SP_RT_LIB) $(SP_RT_MT_LIB) $(SPINEL_TIMEOUT)
	@tmp=$$(mktemp -d /tmp/spinel-gcloc.XXXXXX); ok=1; \
	$(SPINEL) $(GC_LOCALITY_SRC) -o "$$tmp/l" >/dev/null 2>&1 || \
	  { echo "gc-locality-test: FAIL (compile)"; rm -rf "$$tmp"; exit 1; }; \
	for b in 1 2 4; do \
	  for w in 1 8; do \
	    LOCALITY_BUILD=$$b SPINEL_WORKERS=$$w $(TIMEOUT60) "$$tmp/l" > "$$tmp/out.$$b.$$w" 2>/dev/null || \
	      { echo "gc-locality-test: FAIL (build=$$b W=$$w: exited non-zero)"; ok=0; continue; }; \
	    cmp -s "$$tmp/out.$$b.$$w" $(GC_LOCALITY_SRC).expected || \
	      { echo "gc-locality-test: FAIL (build=$$b W=$$w changed the answer: that arm is not the same graph)"; \
	        diff -u $(GC_LOCALITY_SRC).expected "$$tmp/out.$$b.$$w" | head -6; ok=0; }; \
	  done; \
	done; \
	SPINEL_WORKERS=8 LOCALITY_BUILD=4 SPINEL_GC_PHASES=1 SPINEL_GC_MINOR=0 $(TIMEOUT60) "$$tmp/l" >/dev/null 2> "$$tmp/ph.err"; \
	colls=$$(sed -n 's/^\[gc\] \([0-9]*\) collections.*/\1/p' "$$tmp/ph.err" | tail -1); \
	marked=$$(sed -n 's/^\[gcph\] marked \([0-9]*\) objs.*/\1/p' "$$tmp/ph.err" | tail -1); \
	[ -n "$$colls" ] && [ -n "$$marked" ] || \
	  { echo "gc-locality-test: FAIL (no [gc]/[gcph] line under SPINEL_GC_PHASES)"; ok=0; colls=0; marked=0; }; \
	[ "$$colls" -ge 8 ] || \
	  { echo "gc-locality-test: FAIL (only $$colls collections: the churn stopped reaching the collector)"; ok=0; }; \
	awk -v m="$$marked" -v c="$$colls" 'BEGIN{exit !(c > 0 && m / c > 5000)}' || \
	  { echo "gc-locality-test: FAIL (mark walks $$marked objs over $$colls collections: the graph is not being held live)"; ok=0; }; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "gc-locality-test: pass"; else exit 1; fi

# ---- The threaded render benchmark, as a correctness gate (#4384) ----
# benchmark/bm_threaded_render.rb is the only workload in the tree that runs
# the collector under real concurrency: 32 green threads, each holding a
# request's whole render live across a Thread.pass. Everything the threaded
# collector does that the single-threaded one does not -- the parallel slot
# sweep, the per-worker string budget, the root walk over another worker's
# fibers -- is on that path, and three of the last defects found there were
# silent wrong answers rather than crashes.
#
# So the benchmark is run here for its ANSWER, not its time. The digests are
# position-sensitive and fixed by each thread's index, so they do not depend
# on the interleaving: a collection that frees a live fragment, or hands one
# thread another's buffer, changes stdout.
#
# The matrix is worker count crossed with the budget policies, because those
# are what move WHEN a collection lands relative to a half-built page, which
# is the state the bugs were in. SPINEL_WORKERS=1 is included deliberately:
# it is the cooperative model, and a green thread parked at a yield with the
# collector running on the same worker is a different root set from a green
# thread parked while another worker collects.
#
# Two shape assertions keep the leg discriminating rather than merely green.
# A benchmark that stops collecting, or whose live graph shrinks to nothing,
# still produces the right digests -- and would defend nothing.
THREADED_RENDER_SRC := benchmark/bm_threaded_render.rb

threaded-render-test: $(SPINEL) $(SP_RT_LIB) $(SP_RT_MT_LIB) $(SPINEL_TIMEOUT)
	@tmp=$$(mktemp -d /tmp/spinel-thrender.XXXXXX); ok=1; \
	$(SPINEL) $(THREADED_RENDER_SRC) -o "$$tmp/r" >/dev/null 2>&1 || \
	  { echo "threaded-render-test: FAIL (compile)"; rm -rf "$$tmp"; exit 1; }; \
	for w in 1 2 8; do \
	  for mode in default obj walk; do \
	    if [ "$$mode" = default ]; then unset SPINEL_GC_OBJ_BUDGET; \
	    else SPINEL_GC_OBJ_BUDGET=$$mode; export SPINEL_GC_OBJ_BUDGET; fi; \
	    SPINEL_WORKERS=$$w $(TIMEOUT60) "$$tmp/r" > "$$tmp/out.$$w.$$mode" 2>/dev/null || \
	      { echo "threaded-render-test: FAIL (W=$$w $$mode: exited non-zero)"; ok=0; continue; }; \
	    cmp -s "$$tmp/out.$$w.$$mode" $(THREADED_RENDER_SRC).expected || \
	      { echo "threaded-render-test: FAIL (W=$$w $$mode changed the answer)"; \
	        diff -u $(THREADED_RENDER_SRC).expected "$$tmp/out.$$w.$$mode" | head -6; ok=0; }; \
	  done; \
	  unset SPINEL_GC_OBJ_BUDGET; \
	done; \
	SPINEL_WORKERS=8 SPINEL_GC_MINOR=0 $(TIMEOUT60) "$$tmp/r" > "$$tmp/out.minor" 2>/dev/null; \
	cmp -s "$$tmp/out.minor" $(THREADED_RENDER_SRC).expected || \
	  { echo "threaded-render-test: FAIL (SPINEL_GC_MINOR=0 changed the answer)"; ok=0; }; \
	SPINEL_WORKERS=8 SPINEL_GC_PHASES=1 $(TIMEOUT60) "$$tmp/r" >/dev/null 2> "$$tmp/ph.err"; \
	colls=$$(sed -n 's/^\[gc\] \([0-9]*\) collections.*/\1/p' "$$tmp/ph.err" | tail -1); \
	marked=$$(sed -n 's/^\[gcph\] marked \([0-9]*\) objs.*/\1/p' "$$tmp/ph.err" | tail -1); \
	[ -n "$$colls" ] && [ -n "$$marked" ] || \
	  { echo "threaded-render-test: FAIL (no [gc]/[gcph] line under SPINEL_GC_PHASES)"; ok=0; colls=0; marked=0; }; \
	[ "$$colls" -ge 8 ] || \
	  { echo "threaded-render-test: FAIL (only $$colls collections: the workload stopped exercising the collector)"; ok=0; }; \
	awk -v m="$$marked" -v c="$$colls" 'BEGIN{exit !(c > 0 && m / c > 1000)}' || \
	  { echo "threaded-render-test: FAIL (mark walks $$marked objs over $$colls collections: the live graph is gone)"; ok=0; }; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "threaded-render-test: pass"; else exit 1; fi

GC_MINOR_TESTS := test/combinations_yield_ivar.rb \
                  test/gc_minor_thread_local_slot.rb \
                  test/gc_minor_thread_retval.rb \
                  test/str_fresh_recv_rooted.rb \
                  test/gc_minor_thread_tls_first_write.rb \
                  test/proc_cell_capture_marked.rb \
                  test/builtins_minmax.rb \
                  test/gc_minor_byref_lent_slot.rb \
                  test/gc_minor_byref_param_same_name_cell.rb \
                  test/string_handle_group_arity.rb \
                  test/string_handle_eql.rb \
                  test/string_handle_yield_exec.rb \
                  test/gc_minor_barrier_holders.rb \
                  test/bound_method_fresh_receiver.rb \
                  test/issue_2890.rb \
                  test/thread_new_args_rooted_across_fiber_alloc.rb \
                  test/gc_root_frame_slots.rb \
                  test/keyword_splat_rest_copy.rb \
                  test/kw_splat_poly_key_hash.rb \
                  test/poly_array_intersect.rb \
                  test/kw_splat_true_false_nil_operand.rb \
                  test/call_positional_layout.rb \
                  test/block_autosplat_keywords_posts.rb \
                  test/keyword_binding_plan.rb \
                  test/regex_value_as_match_arg.rb \
                  test/kwrest_any_key.rb \
                  test/method_bound_binding_layout.rb \
                  test/bind_call_shapes.rb \
                  test/ivar_recv_before_call_arg.rb \
                  test/reader_operands_pure_read.rb \
                  test/ivar_recv_string_handle.rb \
                  test/string_handle_forward.rb \
                  test/shared_handle_arg_keeps_object.rb \
                  test/index_opassign_fused.rb \
                  test/loop_array_header_cache.rb \
                  test/array_new_fill_sized.rb \
                  test/loop_bounded_index_read.rb \
                  test/loop_bounded_index_polls.rb \
                  test/array_new_block_fresh_binding.rb \
                  test/proc_body_block_fresh_binding.rb \
                  test/hash_new_block_frame.rb \
                  test/kw_splat_boxed_to_hash.rb \
                  test/byref_keyword_rest_splat_param.rb \
                  test/byref_gather_lead_block_super.rb \
                  test/shared_handle_nonunique_callee.rb \
                  test/shared_handle_poly_args.rb \
                  test/instance_exec_args_caller_self.rb \
                  test/class_value_dispatch_args.rb \
                  test/string_handle_proc_method.rb \
                  test/string_handle_bind_dm_curry.rb \
                  test/string_handle_initialize.rb \
                  test/string_handle_poly_variable.rb \
                  test/string_lent_global_slot.rb \
                  test/string_alias_conditional_write.rb \
                  test/string_handle_poly_alias.rb \
                  test/string_handle_splat_gather.rb \
                  test/string_alias_gathered_lead.rb \
                  test/string_gather_optional_post.rb \
                  test/default_reads_callee_self.rb \
                  test/main_body_split.rb \
                  test/string_handle_initialize_kept_block.rb \
                  test/string_handle_captured_param.rb \
                  test/string_alias_yield_block_param.rb \
                  test/string_yield_poly_param_alias.rb \
                  test/string_alias_chain_append.rb \
                  test/string_handle_keyword_args.rb \
                  test/gsub_sub_scan_last_match.rb \
                  test/iter_elem_handed_to_appender.rb \
                  test/iter_block_string_share.rb \
                  test/string_handle_ivar_in_container.rb \
                  test/string_handle_yield_paths.rb \
                  test/string_handle_keyword_dyn_sites.rb

# Each program runs with the minor mark off and on and must answer the same;
# then once more under the generational verifier with stress on (every
# allocation collects, so every survivor promotes), which reports any holder
# the write barrier did not record -- the failure the two answers alone can
# only expose by luck.
GC_MINOR_RESULTS := $(patsubst test/%.rb,build/gc-minor-results/%.res,$(GC_MINOR_TESTS)) \
                    build/gc-minor-results/byref_param_store.chk
gc-minor-test: $(GC_MINOR_RESULTS)
	@ok=1; for r in $(GC_MINOR_RESULTS); do [ "$$(cat $$r)" = 1 ] || ok=0; done; \
	if [ $$ok -eq 1 ]; then echo "gc-minor-test: pass"; else exit 1; fi
# One program per job, in its own temp dir; its FAIL lines print in one piece
# when it is done and its result file records 1 (pass) or 0.
build/gc-minor-results/%.res: test/%.rb FORCE | $(SPINEL) $(SP_RT_LIB) $(SP_RT_MT_LIB) $(SPINEL_TIMEOUT)
	@mkdir -p $(@D); tmp=$$(mktemp -d /tmp/spinel-gcminor.XXXXXX); ok=1; src=$<; \
	{ \
	  bn=$$(basename "$$src" .rb); \
	  if ! $(SPINEL) "$$src" -o "$$tmp/$$bn" >/dev/null 2>&1; then \
	    echo "gc-minor-test: FAIL ($$bn: compile)"; ok=0; \
	  else \
	  for mode in 0 1; do \
	    SPINEL_GC_MINOR=$$mode $(TIMEOUT60) "$$tmp/$$bn" > "$$tmp/$$bn.$$mode" 2>&1; \
	    rc=$$?; \
	    if [ $$rc -ne 0 ]; then echo "gc-minor-test: FAIL ($$bn: SPINEL_GC_MINOR=$$mode exited $$rc)"; tail -3 "$$tmp/$$bn.$$mode"; ok=0; \
	    elif ! cmp -s "$$tmp/$$bn.$$mode" "$$src.expected"; then \
	      echo "gc-minor-test: FAIL ($$bn: SPINEL_GC_MINOR=$$mode output mismatch)"; \
	      diff -u "$$src.expected" "$$tmp/$$bn.$$mode" | head -10; ok=0; fi; \
	  done; \
	  SPINEL_GC_MINOR=1 SPINEL_GC_VERIFY_GEN=1 SPINEL_GC_STRESS=1 $(TIMEOUT60) "$$tmp/$$bn" > "$$tmp/$$bn.stress" 2> "$$tmp/$$bn.verify"; \
	  rc=$$?; \
	  if [ $$rc -ne 0 ]; then \
	    echo "gc-minor-test: FAIL ($$bn: SPINEL_GC_STRESS exited $$rc)"; \
	    tail -3 "$$tmp/$$bn.verify"; ok=0; fi; \
	  if grep -q "generational check" "$$tmp/$$bn.verify"; then \
	    echo "gc-minor-test: FAIL ($$bn: a holder the barrier did not record)"; \
	    head -4 "$$tmp/$$bn.verify"; ok=0; fi; \
	  if ! cmp -s "$$tmp/$$bn.stress" "$$src.expected"; then \
	    echo "gc-minor-test: FAIL ($$bn: output differs under SPINEL_GC_STRESS)"; \
	    diff -u "$$src.expected" "$$tmp/$$bn.stress" | head -10; ok=0; fi; \
	  fi; \
	} > "$$tmp/log" 2>&1; \
	cat "$$tmp/log"; rm -rf "$$tmp"; echo $$ok > $@
build/gc-minor-results/byref_param_store.chk: FORCE | $(SPINEL) $(SP_RT_LIB) $(SPINEL_TIMEOUT)
	@mkdir -p $(@D); tmp=$$(mktemp -d /tmp/spinel-gcminor.XXXXXX); ok=1; \
	$(SPINEL) test/gc_minor_byref_param_same_name_cell.rb --no-line-map -c -o "$$tmp/bp.c" >/dev/null 2>&1; \
	sed -n '/^[^ ].* sp_emit(const char \* \*_cell_io) {$$/,/^}$$/p' "$$tmp/bp.c" > "$$tmp/bp.emit"; \
	if [ ! -s "$$tmp/bp.emit" ] || grep -q sp_gc_wb "$$tmp/bp.emit"; then \
	  echo "gc-minor-test: FAIL (a by-reference parameter's store took a cell barrier: it reads a header off the caller's stack)"; ok=0; fi; \
	rm -rf "$$tmp"; echo $$ok > $@

# ---- Rescued-exception backtrace (#4310) ----
# The frame substrate is a DEBUG build's: sp_bt_enabled is set by a --debug
# main(), and a release build has no symbols to format, so the suite (which
# builds release) can only assert that #backtrace answers an array. This builds
# the fixture the way a debugging user does and checks the chain is really
# there, innermost frame first, with the raising method named.
backtrace-test: $(SPINEL) $(SP_RT_LIB)
	@tmp=$$(mktemp -d /tmp/spinel-bt.XXXXXX); ok=1; \
	$(SPINEL) --debug --no-inline-hot test/backtrace/rescued_chain.rb -o "$$tmp/bt" >/dev/null 2>&1 || \
	  { echo "backtrace-test: FAIL (compile)"; rm -rf "$$tmp"; exit 1; }; \
	"$$tmp/bt" > "$$tmp/out" 2>&1; \
	grep -q "ArgumentError: bad -1" "$$tmp/out" || { echo "backtrace-test: FAIL (the rescue did not run)"; ok=0; }; \
	for f in "Feeder#inner" "Feeder#feed" "Driver#run"; do \
	  grep -q "$$f" "$$tmp/out" || { echo "backtrace-test: FAIL (#4310: frame $$f missing from a rescued backtrace)"; ok=0; }; \
	done; \
	[ "$$(grep -c "rescued_chain.rb" "$$tmp/out")" -ge 4 ] || \
	  { echo "backtrace-test: FAIL (#4310: fewer than 4 frames)"; cat "$$tmp/out"; ok=0; }; \
	head -2 "$$tmp/out" | tail -1 | grep -q "Feeder#inner" || \
	  { echo "backtrace-test: FAIL (#4310: the raising frame is not first)"; cat "$$tmp/out"; ok=0; }; \
	$(SPINEL) --debug --no-inline-hot test/backtrace/pass_through_rescue.rb -o "$$tmp/pt" >/dev/null 2>&1 || \
	  { echo "backtrace-test: FAIL (compile pass_through_rescue)"; ok=0; }; \
	"$$tmp/pt" > "$$tmp/pt.out" 2>&1; \
	for f in "Chain#inner" "Chain#mid" "Chain#outer" "Chain#top"; do \
	  grep -q "$$f" "$$tmp/pt.out" || { echo "backtrace-test: FAIL (#5084: frame $$f cut by a rescue that did not match)"; cat "$$tmp/pt.out"; ok=0; }; \
	done; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "backtrace-test: pass"; else exit 1; fi

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
	tmp=$$(mktemp -d /tmp/spinel-rbscyc.XXXXXX); \
	mkdir -p "$$tmp/d"; ln -s . "$$tmp/d/d"; \
	mkdir -p "$$tmp/b/inner"; ln -s .. "$$tmp/b/inner/up"; \
	mkdir -p "$$tmp/x" "$$tmp/y"; ln -s ../y "$$tmp/x/toy"; ln -s ../x "$$tmp/y/tox"; \
	printf 'class Cyc\n  def a: () -> String\nend\n' > "$$tmp/d/t.rbs"; \
	for t in d b x; do \
	  if [ -n "$$($(RBS_EXTRACT_BIN) "$$tmp/$$t" 2>&1 >/dev/null)" ]; then \
	    echo "rbs-test FAIL: a directory cycle under $$t was walked (#4159)"; fail=1; fi; \
	done; \
	$(RBS_EXTRACT_BIN) "$$tmp/d" 2>/dev/null | grep -q '^class Cyc$$' || \
	  { echo "rbs-test FAIL: an .rbs beside a cycle was not read (#4159)"; fail=1; }; \
	if $(RBS_EXTRACT_BIN) "$$tmp/nosuch" 2>&1 >/dev/null | grep -q 'not found'; then \
	  echo "rbs-test FAIL: stat failure still reported as 'not found' (#4159)"; fail=1; fi; \
	rm -rf "$$tmp"; \
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
RBS_SEED_CHECKS := attr_writer_poly_value dyn_send_arm_seed_contradiction seed_ret_instance_for_class seed_ret_singleton_union hash_or_write_index_setter poly_aset_strbuf_int_arm bare_call_override_unify declared_param_reassigned_poly kw_nil_from_poly_hash inherited_class_keeps_narrowed_ivar nested_ivar nested_array_ivar nested_array_empty_rows nested_array_seed_conflict boundary module_clone_divergent nilable_return byref_string_param shared_handle_nonunique_callee colliding_class_pin return_hash_variant writer_poly_narrowing nilable_scalar_hash_key void_block_tail map_untyped_poly nilable_elem_array_return int_grows_bignum capture_civ_array memo_civ_hash block_param_hash_widen hash_kind_arg_boundary strbuf_ivar_write_value poly_array_ivar pinned_container nilable_arg_group_by inherited_pin_conflict override_family_ret untyped_array_ret yield_union_hash_obj nilable_scalar_ivar nilable_scalar_ret nilable_scalar_arg subclass_into_ancestor_slot seed_check seed_check_bad seed_contradiction seed_contradiction_arg contradicted_returns implicit_conv_no_method typed_slot_block_key typed_slot_compare_obj seeded_param_typed_array_mutation
RBS_SEED_RUN_CHECKS := hash_kind_widened_return poly_dispatch_arm_arg_type nilable_scalar_yield_key nilable_scalar_deep_chain nilable_scalar_paths poly_index_hash_dispatch yield_site_scalar_tail poly_container_op_result untyped_param_two_shapes untyped_recv_string_surface seeded_hash_boundary_values seed_hash_value_kind seed_ret_replaced_def seed_ret_empty_literal untyped_array_ret_from_call nilable_ret_begin_rescue seeded_caller_binds_callee unrelated_setter_seed unrelated_merge_seed seeded_array_store_kind seeded_array_replace_kind seeded_param_poly_array_arg seeded_param_splat_elem seeded_param_nested_call_arg seeded_param_typed_array_arg
RBS_SEED_RESULTS := $(patsubst %,build/rbs-seed-results/%.res,$(RBS_SEED_CHECKS)) \
                    $(patsubst %,build/rbs-seed-results/%.run,$(RBS_SEED_RUN_CHECKS))
rbs-seed-test: $(RBS_SEED_RESULTS)
	@ok=1; for r in $(RBS_SEED_RESULTS); do [ "$$(cat $$r)" = 1 ] || ok=0; done; \
	if [ $$ok -eq 1 ]; then echo "rbs-seed-test: pass"; else exit 1; fi
# The extractor must sit beside $(SPINEL) before any check compiles.
rbs-seed-extractor: $(SPINEL) $(RBS_EXTRACT_BIN)
	@cp -f $(RBS_EXTRACT_BIN) $(dir $(SPINEL))spinel_rbs_extract
# One check per job: each runs in its own temp dir, prints its FAIL lines
# in one piece when done, and records 1 (pass) or 0 in its result file.
build/rbs-seed-results/%.res: FORCE | rbs-seed-extractor $(SP_RT_LIB) $(SPINEL_TIMEOUT)
	@mkdir -p $(@D); tmp=$$(mktemp -d /tmp/spinel-rbsseed.XXXXXX); ok=1; \
	{ case $* in \
	attr_writer_poly_value) \
	$(SPINEL) test/rbs-seed/attr_writer_poly_value.rb --rbs test/rbs-seed/sig -o "$$tmp/awp" >/dev/null 2>&1 && \
	  "$$tmp/awp" > "$$tmp/awp.out" 2>/dev/null && cmp -s "$$tmp/awp.out" test/rbs-seed/attr_writer_poly_value.expected || { echo "rbs-seed-test: FAIL (#4856 a boxed value into an --rbs Integer attr as a method's value)"; ok=0; }; \
	;; \
	dyn_send_arm_seed_contradiction) \
	$(SPINEL) test/rbs-seed/dyn_send_arm_seed_contradiction.rb --rbs test/rbs-seed/sig -o "$$tmp/dasc" >/dev/null 2>&1 && \
	  "$$tmp/dasc" > "$$tmp/dasc.out" 2>/dev/null && cmp -s "$$tmp/dasc.out" test/rbs-seed/dyn_send_arm_seed_contradiction.expected || { echo "rbs-seed-test: FAIL (#6672 a runtime-name send arm contradicting a seeded parameter: refused, or did not raise TypeError when chosen)"; ok=0; }; \
	;; \
	seed_ret_instance_for_class) \
	$(SPINEL) test/rbs-seed/seed_ret_instance_for_class.rb --rbs test/rbs-seed/sig -o "$$tmp/sric" >/dev/null 2>"$$tmp/sric.err" && \
	  "$$tmp/sric" > "$$tmp/sric.out" 2>/dev/null && cmp -s "$$tmp/sric.out" test/rbs-seed/seed_ret_instance_for_class.expected || { echo "rbs-seed-test: FAIL (an instance return seed on a method returning the class itself)"; ok=0; }; \
	[ "$$(grep -c 'returns the class itself' "$$tmp/sric.err")" = 2 ] || { echo "rbs-seed-test: FAIL (an instance return seed contradicting a class-valued body was not reported)"; ok=0; }; \
	;; \
	seed_ret_singleton_union) \
	$(SPINEL) test/rbs-seed/seed_ret_singleton_union.rb --rbs test/rbs-seed/sig -o "$$tmp/srsu" >/dev/null 2>"$$tmp/srsu.err" && \
	  "$$tmp/srsu" > "$$tmp/srsu.out" 2>/dev/null && cmp -s "$$tmp/srsu.out" test/rbs-seed/seed_ret_singleton_union.expected || { echo "rbs-seed-test: FAIL (a singleton(...) return seed)"; ok=0; }; \
	if grep -q 'returns the class itself' "$$tmp/srsu.err"; then echo "rbs-seed-test: FAIL (a singleton(...) union return was reported as declaring an instance, #5036)"; ok=0; fi; \
	;; \
	hash_or_write_index_setter) \
	$(SPINEL) test/rbs-seed/hash_or_write_index_setter.rb --rbs test/rbs-seed/sig -o "$$tmp/hos" >/dev/null 2>&1 && \
	  "$$tmp/hos" > "$$tmp/hos.out" 2>/dev/null && cmp -s "$$tmp/hos.out" test/rbs-seed/hash_or_write_index_setter.expected || { echo "rbs-seed-test: FAIL (#4889 an index write into (@h ||= {}) bound a user []=)"; ok=0; }; \
	;; \
	poly_aset_strbuf_int_arm) \
	$(SPINEL) test/rbs-seed/poly_aset_strbuf_int_arm.rb --rbs test/rbs-seed/sig -o "$$tmp/pas" >/dev/null 2>&1 && \
	  "$$tmp/pas" > "$$tmp/pas.out" 2>/dev/null && cmp -s "$$tmp/pas.out" test/rbs-seed/poly_aset_strbuf_int_arm.expected || { echo "rbs-seed-test: FAIL (#4929 a poly []= handed a String to an Integer-seeded arm)"; ok=0; }; \
	;; \
	bare_call_override_unify) \
	$(SPINEL) test/rbs-seed/bare_call_override_unify.rb --rbs test/rbs-seed/sig -o "$$tmp/bco" >/dev/null 2>&1 && \
	  "$$tmp/bco" > "$$tmp/bco.out" 2>/dev/null && cmp -s "$$tmp/bco.out" test/rbs-seed/bare_call_override_unify.expected || { echo "rbs-seed-test: FAIL (#4600 bare call to an overridden method under a declared return)"; ok=0; }; \
	;; \
	declared_param_reassigned_poly) \
	$(SPINEL) test/rbs-seed/declared_param_reassigned_poly.rb --rbs test/rbs-seed/sig -o "$$tmp/dpr" >/dev/null 2>&1 && \
	  "$$tmp/dpr" > "$$tmp/dpr.out" 2>/dev/null && cmp -s "$$tmp/dpr.out" test/rbs-seed/declared_param_reassigned_poly.expected || { echo "rbs-seed-test: FAIL (#4640 a declared parameter reassigned from a poly value)"; ok=0; }; \
	;; \
	kw_nil_from_poly_hash) \
	$(SPINEL) test/rbs-seed/kw_nil_from_poly_hash.rb --rbs test/rbs-seed/sig -o "$$tmp/knp" >/dev/null 2>&1 && \
	  "$$tmp/knp" > "$$tmp/knp.out" 2>/dev/null && cmp -s "$$tmp/knp.out" test/rbs-seed/kw_nil_from_poly_hash.expected || { echo "rbs-seed-test: FAIL (#5967 a nil keyword read out of a boxed ** into an --rbs Integer? or Float? parameter)"; ok=0; }; \
	;; \
	inherited_class_keeps_narrowed_ivar) \
	$(SPINEL) test/rbs-seed/inherited_class_keeps_narrowed_ivar.rb --rbs test/rbs-seed/sig -o "$$tmp/ick" >/dev/null 2>&1 && \
	  "$$tmp/ick" > "$$tmp/ick.out" 2>/dev/null && cmp -s "$$tmp/ick.out" test/rbs-seed/inherited_class_keeps_narrowed_ivar.expected || { echo "rbs-seed-test: FAIL (#4642 a subclass's narrowed ivar loses its pin in the layout rebuild)"; ok=0; }; \
	;; \
	nested_ivar) \
	$(SPINEL) test/rbs-seed/nested_ivar.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/out.c" 2>/dev/null; \
	grep -Eq 'const char[[:space:]]+\*[[:space:]]*iv_label' "$$tmp/out.c" || { echo "rbs-seed-test: FAIL (#1417: module-nested-class seed not applied)"; ok=0; }; \
	if grep -Eq 'sp_RbVal[[:space:]]+iv_label' "$$tmp/out.c"; then echo "rbs-seed-test: FAIL (#1417: ivar stayed poly)"; ok=0; fi; \
	$(CC) -fsyntax-only -Ilib "$$tmp/out.c" 2>/dev/null || { echo "rbs-seed-test: FAIL (nested_ivar C invalid)"; ok=0; }; \
	;; \
	nested_array_ivar) \
	$(SPINEL) test/rbs-seed/nested_array_ivar.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/nai.c" 2>"$$tmp/nai.err"; \
	grep -Eq 'sp_PtrArray[[:space:]]+\*[[:space:]]*iv_ints' "$$tmp/nai.c" || { echo "rbs-seed-test: FAIL (Array[Array[Integer]] seed did not reach the table)"; ok=0; }; \
	grep -Eq 'sp_PtrArray[[:space:]]+\*[[:space:]]*iv_flts' "$$tmp/nai.c" || { echo "rbs-seed-test: FAIL (Array[Array[Float]] seed did not reach the table)"; ok=0; }; \
	if grep -q 'warning: --rbs' "$$tmp/nai.err"; then echo "rbs-seed-test: FAIL (honoured nested seed still warned)"; sed -n 1,2p "$$tmp/nai.err"; ok=0; fi; \
	if $(SPINEL) test/rbs-seed/nested_array_ivar.rb --rbs test/rbs-seed/sig -o "$$tmp/nai" 2>/dev/null; then \
	  if "$$tmp/nai" > "$$tmp/nai.out" 2>/dev/null; then \
	    cmp -s "$$tmp/nai.out" test/rbs-seed/nested_array_ivar.expected || \
	      { echo "rbs-seed-test: FAIL (nested array seed output mismatch)"; diff -u test/rbs-seed/nested_array_ivar.expected "$$tmp/nai.out" || true; ok=0; }; \
	  else echo "rbs-seed-test: FAIL (nested array seed binary exited non-zero)"; ok=0; fi; \
	else echo "rbs-seed-test: FAIL (nested array seed binary did not build)"; ok=0; fi; \
	;; \
	nested_array_empty_rows) \
	$(SPINEL) test/rbs-seed/nested_array_empty_rows.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/ner.c" 2>"$$tmp/ner.err"; \
	for iv in iv_a iv_b iv_c iv_d; do grep -Eq "sp_PtrArray[[:space:]]+\*[[:space:]]*$$iv" "$$tmp/ner.c" || { echo "rbs-seed-test: FAIL (#4484: nested seed did not supply the row kind for $$iv)"; ok=0; }; done; \
	if grep -q 'warning: --rbs' "$$tmp/ner.err"; then echo "rbs-seed-test: FAIL (#4484: empty-row table still warned)"; sed -n 1,2p "$$tmp/ner.err"; ok=0; fi; \
	if $(SPINEL) test/rbs-seed/nested_array_empty_rows.rb --rbs test/rbs-seed/sig -o "$$tmp/ner" 2>/dev/null; then \
	  if "$$tmp/ner" > "$$tmp/ner.out" 2>/dev/null; then \
	    cmp -s "$$tmp/ner.out" test/rbs-seed/nested_array_empty_rows.expected || \
	      { echo "rbs-seed-test: FAIL (#4484: empty-row table output mismatch)"; diff -u test/rbs-seed/nested_array_empty_rows.expected "$$tmp/ner.out" || true; ok=0; }; \
	  else echo "rbs-seed-test: FAIL (#4484: empty-row table binary exited non-zero)"; ok=0; fi; \
	else echo "rbs-seed-test: FAIL (#4484: empty-row table binary did not build)"; ok=0; fi; \
	;; \
	nested_array_seed_conflict) \
	if $(SPINEL) test/rbs-seed/nested_array_seed_conflict.rb --rbs test/rbs-seed/sig -o "$$tmp/nsc" >"$$tmp/nsc.err" 2>&1; then \
	  echo "rbs-seed-test: FAIL (#4484: a nested seed against rows of the other kind was not refused)"; ok=0; \
	elif ! grep -q 'seed contradicted' "$$tmp/nsc.err"; then \
	  echo "rbs-seed-test: FAIL (#4484: nested seed conflict refused for another reason)"; sed -n 1,2p "$$tmp/nsc.err"; ok=0; fi; \
	;; \
	boundary) \
	$(SPINEL) test/rbs-seed/boundary.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/b.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/b.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/b" 2>"$$tmp/b.err"; then \
	  "$$tmp/b" > "$$tmp/b.out" 2>/dev/null; \
	  cmp -s "$$tmp/b.out" test/rbs-seed/boundary.expected || { echo "rbs-seed-test: FAIL (#1417 boundary output mismatch)"; diff -u test/rbs-seed/boundary.expected "$$tmp/b.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#1417 boundary coercion C did not compile)"; ok=0; fi; \
	;; \
	module_clone_divergent) \
	$(SPINEL) test/rbs-seed/module_clone_divergent.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/mc.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/mc.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/mc" 2>"$$tmp/mc.err"; then \
	  "$$tmp/mc" > "$$tmp/mc.out" 2>/dev/null; \
	  cmp -s "$$tmp/mc.out" test/rbs-seed/module_clone_divergent.expected || { echo "rbs-seed-test: FAIL (#2008 module-clone divergent-hash output mismatch)"; diff -u test/rbs-seed/module_clone_divergent.expected "$$tmp/mc.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#2008 module-clone divergent-hash C did not compile)"; ok=0; fi; \
	;; \
	nilable_return) \
	$(SPINEL) test/rbs-seed/nilable_return.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/nr.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/nr.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/nr" 2>"$$tmp/nr.err"; then \
	  "$$tmp/nr" > "$$tmp/nr.out" 2>/dev/null; \
	  cmp -s "$$tmp/nr.out" test/rbs-seed/nilable_return.expected || { echo "rbs-seed-test: FAIL (#4250 nilable seed erased the nil arm)"; diff -u test/rbs-seed/nilable_return.expected "$$tmp/nr.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#4250 nilable_return C did not compile)"; ok=0; fi; \
	;; \
	byref_string_param) \
	$(SPINEL) test/rbs-seed/byref_string_param.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/br.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/br.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/br" 2>"$$tmp/br.err"; then \
	  "$$tmp/br" > "$$tmp/br.out" 2>/dev/null; \
	  cmp -s "$$tmp/br.out" test/rbs-seed/byref_string_param.expected || { echo "rbs-seed-test: FAIL (a String seed on a mutated param dropped the caller's appends)"; diff -u test/rbs-seed/byref_string_param.expected "$$tmp/br.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (byref_string_param C did not compile)"; ok=0; fi; \
	;; \
	shared_handle_nonunique_callee) \
	$(SPINEL) test/rbs-seed/shared_handle_nonunique_callee.rb --rbs test/rbs-seed/sig -o "$$tmp/shn" >/dev/null 2>&1 && \
	  "$$tmp/shn" > "$$tmp/shn.out" 2>/dev/null && cmp -s "$$tmp/shn.out" test/rbs-seed/shared_handle_nonunique_callee.expected || { echo "rbs-seed-test: FAIL (#6065 an untyped seed's handle lost at a callee whose name two modules define)"; ok=0; }; \
	;; \
	colliding_class_pin) \
	$(SPINEL) test/rbs-seed/colliding_class_pin.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/cp.c" 2>/dev/null; \
	grep -Eq 'const char[[:space:]]*\*[[:space:]]*iv_rtag' "$$tmp/cp.c" || { echo "rbs-seed-test: FAIL (collision-renamed class seed not applied)"; ok=0; }; \
	grep -Eq 'sp_RbVal[[:space:]]+sp_Blue__Base_btag' "$$tmp/cp.c" || { echo "rbs-seed-test: FAIL (poly union return seed not pinned)"; ok=0; }; \
	grep -Eq 'const char[[:space:]]*\*[[:space:]]*iv_itag' "$$tmp/cp.c" || { echo "rbs-seed-test: FAIL (seed for class nested in a renamed class not applied)"; ok=0; }; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/cp.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/cp" 2>"$$tmp/cp.err"; then \
	  "$$tmp/cp" > "$$tmp/cp.out" 2>/dev/null; \
	  cmp -s "$$tmp/cp.out" test/rbs-seed/colliding_class_pin.expected || { echo "rbs-seed-test: FAIL (colliding_class_pin output mismatch)"; diff -u test/rbs-seed/colliding_class_pin.expected "$$tmp/cp.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (colliding_class_pin C did not compile)"; ok=0; fi; \
	;; \
	return_hash_variant) \
	$(SPINEL) test/rbs-seed/return_hash_variant.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/rh.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/rh.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/rh" 2>"$$tmp/rh.err"; then \
	  "$$tmp/rh" > "$$tmp/rh.out" 2>/dev/null; \
	  cmp -s "$$tmp/rh.out" test/rbs-seed/return_hash_variant.expected || { echo "rbs-seed-test: FAIL (#4095 declared-return hash variant output mismatch)"; diff -u test/rbs-seed/return_hash_variant.expected "$$tmp/rh.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#4095 declared-return hash variant C did not compile)"; ok=0; fi; \
	;; \
	writer_poly_narrowing) \
	$(SPINEL) test/rbs-seed/writer_poly_narrowing.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/wp.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/wp.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/wp" 2>"$$tmp/wp.err"; then \
	  "$$tmp/wp" > "$$tmp/wp.out" 2>/dev/null; \
	  cmp -s "$$tmp/wp.out" test/rbs-seed/writer_poly_narrowing.expected || { echo "rbs-seed-test: FAIL (#4093 attr-writer poly narrowing output mismatch)"; diff -u test/rbs-seed/writer_poly_narrowing.expected "$$tmp/wp.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#4093 attr-writer poly narrowing C did not compile)"; ok=0; fi; \
	;; \
	nilable_scalar_hash_key) \
	$(SPINEL) test/rbs-seed/nilable_scalar_hash_key.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/nk.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/nk.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/nk" 2>"$$tmp/nk.err"; then \
	  "$$tmp/nk" > "$$tmp/nk.out" 2>/dev/null; \
	  cmp -s "$$tmp/nk.out" test/rbs-seed/nilable_scalar_hash_key.expected || { echo "rbs-seed-test: FAIL (a nilable scalar seed's nil is a different Hash key than a literal nil)"; diff -u test/rbs-seed/nilable_scalar_hash_key.expected "$$tmp/nk.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (nilable_scalar_hash_key C did not compile)"; ok=0; fi; \
	;; \
	void_block_tail) \
	$(SPINEL) test/rbs-seed/void_block_tail.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/v.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/v.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/v" 2>"$$tmp/v.err"; then \
	  "$$tmp/v" > "$$tmp/v.out" 2>/dev/null; \
	  cmp -s "$$tmp/v.out" test/rbs-seed/void_block_tail.expected || { echo "rbs-seed-test: FAIL (void block tail output mismatch)"; diff -u test/rbs-seed/void_block_tail.expected "$$tmp/v.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (void-returning call as proc tail: C did not compile)"; ok=0; fi; \
	;; \
	map_untyped_poly) \
	$(SPINEL) test/rbs-seed/map_untyped_poly.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/mu.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/mu.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/mu" 2>"$$tmp/mu.err"; then \
	  "$$tmp/mu" > "$$tmp/mu.out" 2>/dev/null; \
	  cmp -s "$$tmp/mu.out" test/rbs-seed/map_untyped_poly.expected || { echo "rbs-seed-test: FAIL (untyped map-into-poly output mismatch)"; diff -u test/rbs-seed/map_untyped_poly.expected "$$tmp/mu.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (untyped map result boxed as sp_box_int: C did not compile)"; ok=0; fi; \
	;; \
	nilable_elem_array_return) \
	$(SPINEL) test/rbs-seed/nilable_elem_array_return.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/nea.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/nea.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/nea" 2>"$$tmp/nea.err"; then \
	  "$$tmp/nea" > "$$tmp/nea.out" 2>/dev/null; \
	  cmp -s "$$tmp/nea.out" test/rbs-seed/nilable_elem_array_return.expected || { echo "rbs-seed-test: FAIL (an Array[Integer] seed CAST a poly array whose element inferred Integer?)"; diff -u test/rbs-seed/nilable_elem_array_return.expected "$$tmp/nea.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (nilable_elem_array_return C did not compile)"; ok=0; fi; \
	;; \
	int_grows_bignum) \
	$(SPINEL) test/rbs-seed/int_grows_bignum.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/ig.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/ig.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/ig" 2>"$$tmp/ig.err"; then \
	  "$$tmp/ig" > "$$tmp/ig.out" 2>/dev/null; \
	  cmp -s "$$tmp/ig.out" test/rbs-seed/int_grows_bignum.expected || { echo "rbs-seed-test: FAIL (an RBS Integer return truncated a bignum body)"; diff -u test/rbs-seed/int_grows_bignum.expected "$$tmp/ig.out" | head -20; ok=0; }; \
	else echo "rbs-seed-test: FAIL (int_grows_bignum C did not compile)"; ok=0; fi; \
	;; \
	capture_civ_array) \
	$(SPINEL) test/rbs-seed/capture_civ_array.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/cca.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/cca.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/cca" 2>"$$tmp/cca.err"; then \
	  "$$tmp/cca" > "$$tmp/cca.out" 2>/dev/null; \
	  cmp -s "$$tmp/cca.out" test/rbs-seed/capture_civ_array.expected || { echo "rbs-seed-test: FAIL (#1827 typed-array return pin output mismatch)"; diff -u test/rbs-seed/capture_civ_array.expected "$$tmp/cca.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#1827 Array[String] return pin: C did not compile)"; ok=0; fi; \
	;; \
	memo_civ_hash) \
	$(SPINEL) test/rbs-seed/memo_civ_hash.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/mh.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/mh.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/mh" 2>"$$tmp/mh.err"; then \
	  "$$tmp/mh" > "$$tmp/mh.out" 2>/dev/null; \
	  cmp -s "$$tmp/mh.out" test/rbs-seed/memo_civ_hash.expected || { echo "rbs-seed-test: FAIL (#3779 memoized class-ivar hash pin output mismatch)"; diff -u test/rbs-seed/memo_civ_hash.expected "$$tmp/mh.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#3779 memoized class-ivar hash pin: C did not compile)"; ok=0; fi; \
	;; \
	block_param_hash_widen) \
	$(SPINEL) test/rbs-seed/block_param_hash_widen.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/bw.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/bw.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/bw" 2>"$$tmp/bw.err"; then \
	  "$$tmp/bw" > "$$tmp/bw.out" 2>/dev/null; \
	  cmp -s "$$tmp/bw.out" test/rbs-seed/block_param_hash_widen.expected || { echo "rbs-seed-test: FAIL (#4100 block param over an untyped receiver widened the hash it writes into)"; diff -u test/rbs-seed/block_param_hash_widen.expected "$$tmp/bw.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#4100 widened block param: C did not compile)"; ok=0; fi; \
	;; \
	hash_kind_arg_boundary) \
	$(SPINEL) test/rbs-seed/hash_kind_arg_boundary.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/hk.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/hk.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/hk" 2>"$$tmp/hk.err"; then \
	  "$$tmp/hk" > "$$tmp/hk.out" 2>/dev/null; \
	  cmp -s "$$tmp/hk.out" test/rbs-seed/hash_kind_arg_boundary.expected || { echo "rbs-seed-test: FAIL (#3994 hash-kind argument boundary output mismatch)"; diff -u test/rbs-seed/hash_kind_arg_boundary.expected "$$tmp/hk.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#3994 hash-kind argument boundary: C did not compile)"; ok=0; fi; \
	;; \
	strbuf_ivar_write_value) \
	$(SPINEL) test/rbs-seed/strbuf_ivar_write_value.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/sw.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/sw.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/sw" 2>"$$tmp/sw.err"; then \
	  "$$tmp/sw" > "$$tmp/sw.out" 2>/dev/null; \
	  cmp -s "$$tmp/sw.out" test/rbs-seed/strbuf_ivar_write_value.expected || { echo "rbs-seed-test: FAIL (#3993 strbuf ivar write-value output mismatch)"; diff -u test/rbs-seed/strbuf_ivar_write_value.expected "$$tmp/sw.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#3993 strbuf ivar write in value position: C did not compile)"; ok=0; fi; \
	;; \
	poly_array_ivar) \
	$(SPINEL) test/rbs-seed/poly_array_ivar.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/pa.c" 2>/dev/null; \
	grep -Eq 'sp_PolyArray[[:space:]]*\*[[:space:]]*iv_kids' "$$tmp/pa.c" || { echo "rbs-seed-test: FAIL (poly_array ivar seed dropped)"; ok=0; }; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/pa.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/pa" 2>"$$tmp/pa.err"; then \
	  "$$tmp/pa" > "$$tmp/pa.out" 2>/dev/null; \
	  cmp -s "$$tmp/pa.out" test/rbs-seed/poly_array_ivar.expected || { echo "rbs-seed-test: FAIL (poly_array ivar output mismatch)"; ok=0; }; \
	else echo "rbs-seed-test: FAIL (poly_array ivar: C did not compile)"; ok=0; fi; \
	;; \
	pinned_container) \
	$(SPINEL) test/rbs-seed/pinned_container.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/pc.c" 2>/dev/null; \
	grep -Eq 'sp_PolyArray[[:space:]]*\*[[:space:]]*iv_kids' "$$tmp/pc.c" || { echo "rbs-seed-test: FAIL (ivar seed pin lost to fixpoint inference)"; ok=0; }; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/pc.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/pc" 2>"$$tmp/pc.err"; then \
	  "$$tmp/pc" > "$$tmp/pc.out" 2>/dev/null; \
	  cmp -s "$$tmp/pc.out" test/rbs-seed/pinned_container.expected || { echo "rbs-seed-test: FAIL (pinned container output mismatch)"; diff -u test/rbs-seed/pinned_container.expected "$$tmp/pc.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (pinned container: C did not compile)"; ok=0; fi; \
	;; \
	nilable_arg_group_by) \
	$(SPINEL) test/rbs-seed/nilable_arg_group_by.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/gb.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/gb.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/gb" 2>"$$tmp/gb.err"; then \
	  "$$tmp/gb" > "$$tmp/gb.out" 2>/dev/null; \
	  cmp -s "$$tmp/gb.out" test/rbs-seed/nilable_arg_group_by.expected || { echo "rbs-seed-test: FAIL (#2438 nilable-arg group_by output mismatch)"; diff -u test/rbs-seed/nilable_arg_group_by.expected "$$tmp/gb.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#2438 nilable-arg group_by: C did not compile)"; ok=0; fi; \
	;; \
	inherited_pin_conflict) \
	$(SPINEL) test/rbs-seed/inherited_pin_conflict.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/ipc.c" 2>"$$tmp/ipc.warn"; \
	grep -q "ivar pin @id dropped on Thing" "$$tmp/ipc.warn" || { echo "rbs-seed-test: FAIL (#1871 conflicting inherited pin didn't warn)"; ok=0; }; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/ipc.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/ipc" 2>"$$tmp/ipc.err"; then \
	  "$$tmp/ipc" > "$$tmp/ipc.out" 2>/dev/null; \
	  cmp -s "$$tmp/ipc.out" test/rbs-seed/inherited_pin_conflict.expected || { echo "rbs-seed-test: FAIL (#1871 inherited-pin output mismatch)"; diff -u test/rbs-seed/inherited_pin_conflict.expected "$$tmp/ipc.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#1871 inherited pin conflict: C did not compile)"; ok=0; fi; \
	;; \
	override_family_ret) \
	$(SPINEL) test/rbs-seed/override_family_ret.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/ofr.c" 2>/dev/null; \
	$(CC) -fsyntax-only -Ilib "$$tmp/ofr.c" 2>/dev/null || { echo "rbs-seed-test: FAIL (#3203 override-family return seed split decl/call-site repr)"; ok=0; }; \
	;; \
	untyped_array_ret) \
	$(SPINEL) test/rbs-seed/untyped_array_ret.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/ua.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/ua.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/ua" 2>"$$tmp/ua.err"; then \
	  "$$tmp/ua" > "$$tmp/ua.out" 2>/dev/null; \
	  cmp -s "$$tmp/ua.out" test/rbs-seed/untyped_array_ret.expected || { echo "rbs-seed-test: FAIL (#3279 untyped-array return output mismatch)"; diff -u test/rbs-seed/untyped_array_ret.expected "$$tmp/ua.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#3279 untyped-array return: C did not compile)"; ok=0; fi; \
	;; \
	yield_union_hash_obj) \
	$(SPINEL) test/rbs-seed/yield_union_hash_obj.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/yu.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/yu.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/yu" 2>"$$tmp/yu.err"; then \
	  "$$tmp/yu" > "$$tmp/yu.out" 2>/dev/null; \
	  cmp -s "$$tmp/yu.out" test/rbs-seed/yield_union_hash_obj.expected || { echo "rbs-seed-test: FAIL (#3278 yield-union output mismatch)"; diff -u test/rbs-seed/yield_union_hash_obj.expected "$$tmp/yu.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#3278 yield-union: C did not compile)"; ok=0; fi; \
	;; \
	nilable_scalar_ivar) \
	$(SPINEL) test/rbs-seed/nilable_scalar_ivar.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/ns.c" 2>/dev/null; \
	grep -Eq 'sp_RbVal[[:space:]]+iv_f;' "$$tmp/ns.c" || { echo "rbs-seed-test: FAIL (#3412: bool? pinned a slot with no nil)"; ok=0; }; \
	grep -Eq 'sp_RbVal[[:space:]]+iv_y;' "$$tmp/ns.c" || { echo "rbs-seed-test: FAIL (#3412: Symbol? pinned a slot with no nil)"; ok=0; }; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/ns.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/ns" 2>"$$tmp/ns.err"; then \
	  "$$tmp/ns" > "$$tmp/ns.out" 2>/dev/null; \
	  cmp -s "$$tmp/ns.out" test/rbs-seed/nilable_scalar_ivar.expected || { echo "rbs-seed-test: FAIL (#3412 nilable-scalar ivar output mismatch)"; diff -u test/rbs-seed/nilable_scalar_ivar.expected "$$tmp/ns.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#3412 nilable-scalar ivar: C did not compile)"; ok=0; fi; \
	;; \
	nilable_scalar_ret) \
	$(SPINEL) test/rbs-seed/nilable_scalar_ret.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/nr.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/nr.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/nr" 2>"$$tmp/nr.err"; then \
	  "$$tmp/nr" > "$$tmp/nr.out" 2>/dev/null; \
	  cmp -s "$$tmp/nr.out" test/rbs-seed/nilable_scalar_ret.expected || { echo "rbs-seed-test: FAIL (#3458 nilable-scalar return output mismatch)"; diff -u test/rbs-seed/nilable_scalar_ret.expected "$$tmp/nr.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#3458 nilable-scalar return: C did not compile)"; ok=0; fi; \
	;; \
	nilable_scalar_arg) \
	$(SPINEL) test/rbs-seed/nilable_scalar_arg.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/na.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/na.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/na" 2>"$$tmp/na.err"; then \
	  "$$tmp/na" > "$$tmp/na.out" 2>/dev/null; \
	  cmp -s "$$tmp/na.out" test/rbs-seed/nilable_scalar_arg.expected || { echo "rbs-seed-test: FAIL (#3465 nilable-scalar arg output mismatch)"; diff -u test/rbs-seed/nilable_scalar_arg.expected "$$tmp/na.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#3465 nilable-scalar arg: C did not compile)"; ok=0; fi; \
	;; \
	subclass_into_ancestor_slot) \
	$(SPINEL) test/rbs-seed/subclass_into_ancestor_slot.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/sa.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib -Werror=incompatible-pointer-types "$$tmp/sa.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/sa" 2>"$$tmp/sa.err"; then \
	  "$$tmp/sa" > "$$tmp/sa.out" 2>/dev/null; \
	  cmp -s "$$tmp/sa.out" test/rbs-seed/subclass_into_ancestor_slot.expected || { echo "rbs-seed-test: FAIL (#3418 ancestor-slot output mismatch)"; diff -u test/rbs-seed/subclass_into_ancestor_slot.expected "$$tmp/sa.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#3418: emitted C is not pointer-typeclean -- GCC 14+ rejects it outright)"; sed -n 1,20p "$$tmp/sa.err"; ok=0; fi; \
	;; \
	seed_check) \
	$(SPINEL) test/rbs-seed/seed_check.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/sk.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) -DSP_RBS_CHECK "$$tmp/sk.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/sk" 2>"$$tmp/sk.err"; then \
	  "$$tmp/sk" > "$$tmp/sk.out" 2>&1; \
	  cmp -s "$$tmp/sk.out" test/rbs-seed/seed_check.expected || { echo "rbs-seed-test: FAIL (seed check fired on an honest seed)"; diff -u test/rbs-seed/seed_check.expected "$$tmp/sk.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (seed_check: C did not compile)"; ok=0; fi; \
	;; \
	seed_check_bad) \
	$(SPINEL) test/rbs-seed/seed_check_bad.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/skb.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) -DSP_RBS_CHECK "$$tmp/skb.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/skb" 2>"$$tmp/skb.err"; then \
	  if "$$tmp/skb" > "$$tmp/skb.out" 2>&1; then echo "rbs-seed-test: FAIL (a contradicted seed did NOT abort under -DSP_RBS_CHECK)"; ok=0; \
	  else grep -q "seed violated" "$$tmp/skb.out" || { echo "rbs-seed-test: FAIL (contradicted seed aborted without naming the seed)"; sed -n 1,5p "$$tmp/skb.out"; ok=0; }; fi; \
	else echo "rbs-seed-test: FAIL (seed_check_bad: C did not compile)"; ok=0; fi; \
	;; \
	seed_contradiction) \
	if $(SPINEL) test/rbs-seed/seed_contradiction.rb --rbs test/rbs-seed/sig \
	     -c --no-line-map -o "$$tmp/sx.c" >"$$tmp/sx.out" 2>&1; then \
	  echo "rbs-seed-test: FAIL (a statically contradicted seed compiled)"; ok=0; \
	else grep -q "seed contradicted" "$$tmp/sx.out" || { echo "rbs-seed-test: FAIL (contradicted seed rejected without saying why)"; sed -n 1,5p "$$tmp/sx.out"; ok=0; }; fi; \
	;; \
	seed_contradiction_arg) \
	if $(SPINEL) test/rbs-seed/seed_contradiction_arg.rb --rbs test/rbs-seed/sig \
	     -c --no-line-map -o "$$tmp/sxa.c" >"$$tmp/sxa.out" 2>&1; then \
	  echo "rbs-seed-test: FAIL (a contradicted seed on an ARGUMENT compiled)"; ok=0; \
	else grep -q "seed contradicted" "$$tmp/sxa.out" || { echo "rbs-seed-test: FAIL (contradicted argument rejected without saying why)"; sed -n 1,5p "$$tmp/sxa.out"; ok=0; }; fi; \
	;; \
	contradicted_returns) \
	for t in seed_contradiction_ret seed_contradiction_ret_obj seed_hash_key_kind seed_array_elem_kind; do \
	  if $(SPINEL) test/rbs-seed/$$t.rb --rbs test/rbs-seed/sig \
	       -c --no-line-map -o "$$tmp/$$t.c" >"$$tmp/$$t.out" 2>&1; then \
	    echo "rbs-seed-test: FAIL (a contradicted RETURN seed compiled: $$t)"; ok=0; \
	  else grep -q "seed contradicted" "$$tmp/$$t.out" || { echo "rbs-seed-test: FAIL ($$t rejected without saying why)"; sed -n 1,5p "$$tmp/$$t.out"; ok=0; }; fi; \
	done; \
	;; \
	implicit_conv_no_method) \
	if $(SPINEL) test/rbs-seed/implicit_conv_no_method.rb \
	     -c --no-line-map -o "$$tmp/icnm.c" >"$$tmp/icnm.out" 2>&1; then \
	  echo "rbs-seed-test: FAIL (an object with no #to_str compiled into a String slot)"; ok=0; \
	else grep -q "no implicit conversion of Inert into String" "$$tmp/icnm.out" || { echo "rbs-seed-test: FAIL (missing #to_str rejected without saying why)"; sed -n 1,5p "$$tmp/icnm.out"; ok=0; }; fi; \
	;; \
	typed_slot_block_key) \
	if $(SPINEL) test/rbs-seed/typed_slot_block_key.rb \
	     -c --no-line-map -o "$$tmp/tsbk.c" >"$$tmp/tsbk.out" 2>&1; then \
	  echo "rbs-seed-test: FAIL (a foreign key reached a typed block parameter)"; ok=0; \
	else grep -q "a key of another class than the hash's keys" "$$tmp/tsbk.out" || { echo "rbs-seed-test: FAIL (foreign block key rejected without saying why)"; sed -n 1,5p "$$tmp/tsbk.out"; ok=0; }; fi; \
	;; \
	typed_slot_compare_obj) \
	if $(SPINEL) test/rbs-seed/typed_slot_compare_obj.rb \
	     -c --no-line-map -o "$$tmp/tsco.c" >"$$tmp/tsco.out" 2>&1; then \
	  echo "rbs-seed-test: FAIL (a comparing user object reached a typed Array slot)"; ok=0; \
	else grep -q "a user object defining == compared against a typed Array" "$$tmp/tsco.out" || { echo "rbs-seed-test: FAIL (comparing object rejected without saying why)"; sed -n 1,5p "$$tmp/tsco.out"; ok=0; }; fi; \
	;; \
	seeded_param_typed_array_mutation) \
	if $(SPINEL) test/rbs-seed/seeded_param_typed_array_mutation.rb --rbs test/rbs-seed/sig \
	     -c --no-line-map -o "$$tmp/stam.c" >"$$tmp/stam.out" 2>&1; then \
	  echo "rbs-seed-test: FAIL (a seeded general-Array parameter lost a held typed array's mutation)"; ok=0; \
	else grep -q "which the method mutates" "$$tmp/stam.out" || { echo "rbs-seed-test: FAIL (seeded array mutation rejected without saying why)"; sed -n 1,5p "$$tmp/stam.out"; ok=0; }; fi; \
	;; \
	*) echo "rbs-seed-test: FAIL (no check named $*)"; ok=0 ;; \
	esac; } > "$$tmp/log" 2>&1; \
	cat "$$tmp/log"; rm -rf "$$tmp"; echo $$ok > $@
build/rbs-seed-results/%.run: FORCE | rbs-seed-extractor $(SP_RT_LIB) $(SPINEL_TIMEOUT)
	@mkdir -p $(@D); tmp=$$(mktemp -d /tmp/spinel-rbsseed.XXXXXX); ok=1; t=$*; \
	{ \
	  $(SPINEL) test/rbs-seed/$$t.rb --rbs test/rbs-seed/sig -c --no-line-map -o "$$tmp/$$t.c" 2>/dev/null; \
	  if $(CC) -O0 -Ilib $(RBS_SEED_STRICT) "$$tmp/$$t.c" $(SP_RT_LIB) $(LDFLAGS) -lm -o "$$tmp/$$t" 2>"$$tmp/$$t.err"; then \
	    "$$tmp/$$t" > "$$tmp/$$t.out" 2>/dev/null; \
	    cmp -s "$$tmp/$$t.out" test/rbs-seed/$$t.expected || { echo "rbs-seed-test: FAIL ($$t output mismatch)"; diff -u test/rbs-seed/$$t.expected "$$tmp/$$t.out" || true; ok=0; }; \
	  else echo "rbs-seed-test: FAIL ($$t: C did not compile)"; sed -n 1,10p "$$tmp/$$t.err"; ok=0; fi; \
	} > "$$tmp/log" 2>&1; \
	cat "$$tmp/log"; rm -rf "$$tmp"; echo $$ok > $@
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
# The generated C names the external libraries its ffi_lib declarations asked
# for, as `/* SPINEL_LINK: -lfoo */` markers -- the same ones the spinel driver
# scrapes when it drives cc itself. This rule drives cc directly, so it has to
# read them too, or a package binding a system library (openssl) fails to link.
# Link what the driver would link. A TU that uses Thread carries codegen's
# /* SPINEL_USES_THREADS */ marker, and src/main.c answers it with the
# -DSP_THREADS archive plus -lpthread; the harness used to link the N=1
# cooperative archive for every test, so every threaded test ran in a
# configuration that never ships -- and a test whose main thread blocks in a
# syscall while another green thread must make progress deadlocked here and
# nowhere else. The PCH is dropped on that path: it was built without
# -DSP_THREADS and -Werror rejects the mismatch.
# `bigopt` below: -O1 beats -O0 on a typical test, because the optimizer prunes
# spinel_rt.h's 800+ unreferenced statics before codegen (see the OPT=-O1 note on
# `check`). But the cost of the passes that stay is superlinear in the size of
# ONE function, and a test's whole program is inlined into main. io_closed_stream
# has a 5373-line main: 82s at -O1 against 1.6s at -O0, with a roughly cubic
# curve through it (928 lines 0.9s, 3219 lines 15s, 4407 lines 43s). Past 2000
# generated lines that one function dominates and -O0 wins by a wide margin. The
# PCH is dropped with it, since it was built under the other -O and would not
# load anyway. The binaries are the same speed here -- these are wide-API tests
# rather than loops, 0.023s vs 0.024s on the worst one.
#
# The result cache (tools/result_cache.sh). spinel always runs: the C it emits
# is how the harness knows whether anything changed, and most commits change
# the C of few programs (a refactor, none). When it is the same C as in a run
# that PASSED, built the same way against the same runtime, with the same
# expectations, the compile and the run are skipped and the stored PASS is
# reused. The key of one program is the hash of
#   - the generated C text and the .rb source;
#   - its .expected, .err.expected, .args and .stdin (absent counts too);
#   - the compile and link line as this recipe assembles it for THIS program
#     ($(CC), CFLAGS/OPT, the -O0 override, the overflow and thread defines,
#     the PCH flag, the archive and package objects picked, -l flags), and the
#     timeout;
#   - RESULT_CACHE_FP, computed once per run after the prerequisites are
#     built: the content of both runtime archives, every bundled package
#     object, both PCH files and build/spinel-timeout, every header under lib/
#     and packages/, `$(CC) --version` and -dumpmachine, the OS and
#     architecture, and LD_LIBRARY_PATH/LIBRARY_PATH;
#   - RESULT_CACHE_HARNESS: bump it when this recipe's logic changes how a
#     result is decided.
# Only PASS is stored, never a failure, so a cached result can only repeat a
# pass that this exact binary, run against these exact expectations, earned.
# What a hit assumes:
#   - spinel affects the outcome only through the C it writes (the C is
#     hashed, not the compiler binary, which is the point);
#   - the binary is a function of the key: the same C, headers, PCH, archives,
#     objects, flags and compiler give the same program. libc, the system
#     libraries a test links (-lssl, -lffi) and the kernel are out of scope;
#   - the run is a function of the binary and its inputs. That does not hold
#     for a program that reads the clock, the environment, the random source,
#     files (a fixture a commit edits leaves the C alone), the network, other
#     processes or threads' timing, or the GC's counters, so such a program is
#     never cached: tools/result_cache.sh's `nocache` scans the test and every
#     local file it requires for those identifiers, and `# spinel: no-cache` in
#     a test opts out anything the scan does not see. A test without
#     .expected compares against CRuby's run and is never cached either.
# A reused pass leaves <test>.ok.cached beside the .ok, so
# `ls build/test-results/*.cached | wc -l` counts what was skipped.
# GATE_CACHE=0 (or `make gate-full`) runs everything. The entries live in
# build/result-cache, one file per key written by rename, so concurrent runs
# and configurations (CC=clang, -m32, OPT) share it safely; entries unused for
# two weeks are pruned when the test results are cleared.
GATE_CACHE ?= 1
export GATE_CACHE
RESULT_CACHE_HARNESS := 1
RESULT_CACHE_FP = $(if $(filter 0,$(GATE_CACHE)),off,$(eval RESULT_CACHE_FP := $(shell RC_CC="$(CC)" tools/result_cache.sh fp $(SP_RT_LIB) $(SP_RT_MT_LIB) $(BUNDLED_NATIVE_OBJS) $(BUNDLED_NATIVE_MT_OBJS) $(PCH_PLAIN) $(PCH_NOPOLY) $(SPINEL_TIMEOUT)))$(RESULT_CACHE_FP))
define RUN_ONE_TEST
@mkdir -p build/test-results
@# Raise the descriptor soft limit toward the hard one, best effort. A test
@# that has to reach a descriptor past FD_SETSIZE (io_select_high_fd, #4314)
@# cannot get there under a 1024 soft limit, and a shell that refuses the
@# raise leaves the test to fall back to whatever it can open.
@ulimit -n 4096 2>/dev/null || true; \
tmpdir=$$(mktemp -d /tmp/spinel-test.XXXXXX); \
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
rm -f "$@.diff" "$@.cached"; \
ckey=""; hit=0; \
$(SPINEL) "$<" $(SP_OV_FLAG) -c --no-line-map -o "$$cfile" 2>/dev/null && \
{ pchuse="$(PCH_USE_PLAIN)"; pchf="$(PCH_PLAIN)"; \
  if head -2 "$$cfile" | grep -q SP_TU_NO_POLY_RENDER; then pchuse="$(PCH_USE_NOPOLY)"; pchf="$(PCH_NOPOLY)"; fi; \
  [ -f "$$pchf" ] || pchuse=""; \
  xlibs=$$(sed -n 's|^/\* SPINEL_LINK: \(.*\) \*/$$|\1|p' "$$cfile" | tr '\n' ' '); \
  bigopt=""; \
  if [ "$$(wc -l < "$$cfile")" -ge 2000 ]; then bigopt="-O0"; pchuse=""; fi; \
  mtdef=""; rtlib="$(SP_RT_LIB)"; natobjs="$(BUNDLED_NATIVE_OBJS)"; mtld=""; \
  if grep -q SPINEL_USES_THREADS "$$cfile"; then \
    mtdef="$(MT_DEF)"; rtlib="$(SP_RT_MT_LIB)"; natobjs="$(BUNDLED_NATIVE_MT_OBJS)"; mtld="-lpthread"; pchuse=""; \
  fi; \
  if [ -f "$<.expected" ]; then \
    ckey=$$(RC_CC="$(CC)" tools/result_cache.sh key "$<" "$$cfile" \
      "$(RESULT_CACHE_FP)|$(RESULT_CACHE_HARNESS)|$(CC)|$(TEST_SINGLE_INVOKE)|$(CFLAGS) $$bigopt $(SP_OV_DEFINE) $$mtdef -Werror $(TEST_WARN_SUPPRESS) $(SEC_FLAGS) $$pchuse -Ilib|$$natobjs $$rtlib $(LDFLAGS) -lm $$mtld $$xlibs $(GC_FLAGS)|$(TIMEOUT10)" \
      "$<.expected" "$<.err.expected" "$<.args" "$<.stdin"); \
    if [ -n "$$ckey" ] && tools/result_cache.sh get "$$ckey" 2>/dev/null | grep -qx PASS; then hit=1; fi; \
  fi; \
  if [ $$hit = 1 ]; then \
    :; \
  elif [ -n "$(TEST_SINGLE_INVOKE)" ]; then \
    $(CC) $(CFLAGS) $$bigopt $(SP_OV_DEFINE) $$mtdef -Werror $(TEST_WARN_SUPPRESS) $(SEC_FLAGS) $$pchuse -Ilib "$$cfile" $$natobjs $$rtlib $(LDFLAGS) -lm $$mtld $$xlibs $(GC_FLAGS) -o "$$bin" 2>/dev/null; \
  else \
    $(CC) $(CFLAGS) $$bigopt $(SP_OV_DEFINE) $$mtdef -Werror $(TEST_WARN_SUPPRESS) $(SEC_FLAGS) $$pchuse -Ilib -c "$$cfile" -o "$$cfile.o" 2>/dev/null && \
    $(CC) $(CFLAGS) "$$cfile.o" $$natobjs $$rtlib $(LDFLAGS) -lm $$mtld $$xlibs $(GC_FLAGS) -o "$$bin" 2>/dev/null; \
  fi; }; \
built=$$?; \
if [ $$built -eq 0 ] && [ $$hit = 1 ]; then \
  echo PASS > "$@"; : > "$@.cached"; \
  if [ -t 1 ]; then printf .; fi; \
elif [ $$built -eq 0 ]; then \
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
  $(TIMEOUT10) "$$bin" $$args <"$$stdinf" >"$$act" 2>"$$acterr"; run_rc=$$?; \
  LC_ALL=C sed 's/\r$$//' "$$act" >"$$act.n"; \
  LC_ALL=C sed 's/\r$$//' "$$acterr" >"$$acterr.n"; \
  if [ -f "$<.err.expected" ]; then \
    LC_ALL=C sed 's/\r$$//' "$<.err.expected" >"$$experr.n"; \
  else \
    : > "$$experr.n"; \
  fi; \
  if cmp -s "$$exp.n" "$$act.n" && cmp -s "$$experr.n" "$$acterr.n"; then \
    echo PASS > "$@"; \
    if [ -n "$$ckey" ]; then echo PASS | tools/result_cache.sh put "$$ckey"; fi; \
    if [ -t 1 ]; then printf .; fi; \
  else \
    echo FAIL > "$@"; \
    { if [ "$$run_rc" -eq 124 ]; then echo "=== timed out after 10s (the output below is partial) ==="; fi; \
      echo "=== stdout diff (expected vs actual) ==="; diff -u "$$exp.n" "$$act.n" || true; \
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
build/test-results/pkg.$(1).%.ok: packages/$(1)/test/%.rb $$(SP_RT_LIB) $$(SP_RT_MT_LIB) $$(BUNDLED_NATIVE_OBJS) $$(BUNDLED_NATIVE_MT_OBJS) $$(PCH_PLAIN) $$(PCH_NOPOLY) | $$(SPINEL) $$(SPINEL_TIMEOUT)
	$$(RUN_ONE_TEST)
endef
$(foreach d,$(wildcard packages/*/test),$(eval $(call PKG_TEST_RULE,$(patsubst packages/%/test,%,$(d)))))

build/test-results/%.ok: test/%.rb $(SP_RT_LIB) $(SP_RT_MT_LIB) $(BUNDLED_NATIVE_OBJS) $(BUNDLED_NATIVE_MT_OBJS) $(PCH_PLAIN) $(PCH_NOPOLY) | $(SPINEL) $(SPINEL_TIMEOUT)
	$(RUN_ONE_TEST)

clean-test-results:
	@rm -rf build/test-results
	@tools/result_cache.sh prune

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
benchmark/%.rb.expected: benchmark/%.rb | $(SPINEL_TIMEOUT)
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
# Compile-time scaling (#4847): spinel's analysis and C emission on the
# synthetic program of tools/compile_scale_gen.rb at two sizes, with the growth
# between them. Read the ratio, not the seconds (the program is linear in K).
# `ruby tools/compile_scale.rb --cc --check K...` also builds and checks.
BENCH_COMPILE_K ?= 100 200
.PHONY: bench-compile
bench-compile: $(SPINEL)
	@ruby tools/compile_scale.rb $(BENCH_COMPILE_K)

bench: $(SPINEL) $(SP_RT_LIB) $(SP_RT_MT_LIB) $(SPINEL_TIMEOUT)
	@if [ -z "$(TIMEOUT_BIN)" ]; then echo "Note: no 'timeout' command found; running without time limits."; fi
	@rm -rf build/bench-results; mkdir -p build/bench-results
	@ls benchmark/*.rb | xargs -P $(BENCH_PJOBS) -n 1 sh -c '\
	  f="$$1"; bn=$$(basename "$$f" .rb); d=build/bench-results; res="$$d/$$bn.res"; \
	  c="$$d/$$bn.c"; o="$$d/$$bn.o"; bin="$$d/$$bn.bin"; exp="$$d/$$bn.exp"; act="$$d/$$bn.act"; \
	  mtdef=""; rtlib="$(SP_RT_LIB)"; natobjs="$(BUNDLED_NATIVE_OBJS)"; mtld=""; \
	  if $(TIMEOUT10) $(SPINEL) "$$f" -c --no-line-map -o "$$c" 2>/dev/null; then \
	    if grep -q SPINEL_USES_THREADS "$$c"; then \
	      mtdef="$(MT_DEF)"; rtlib="$(SP_RT_MT_LIB)"; natobjs="$(BUNDLED_NATIVE_MT_OBJS)"; mtld="-lpthread"; \
	    fi; \
	  fi; \
	  if [ -f "$$c" ] \
	     && $(CC) $(CFLAGS) $$mtdef -Werror $(TEST_WARN_SUPPRESS) $(SEC_FLAGS) -Ilib -c "$$c" -o "$$o" 2>/dev/null \
	     && $(CC) $(CFLAGS) "$$o" $$natobjs $$rtlib $(LDFLAGS) -lm $$mtld $(GC_FLAGS) -o "$$bin" 2>/dev/null; then \
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
	  rm -rf build/rubyspec-ex-$$nm && ruby tools/rubyspec/extract.rb $(RUBYSPEC_DIR)/$$d build/rubyspec-ex-$$nm || exit 1; \
	  bash tools/rubyspec/run.sh build/rubyspec-ex-$$nm build/rubyspec-results-$$nm.tsv; \
	  ruby tools/rubyspec/manifest_diff.rb tools/rubyspec/expectations/$$nm.tsv build/rubyspec-results-$$nm.tsv || true; \
	done

# Retention gate: re-run only the examples the manifests expect to PASS and
# fail on any regression. Improvements (non-PASS -> PASS) never fail this
# target -- they surface in `make rubyspec`'s manifest diff instead, and are
# promoted by regenerating the manifest deliberately.
# A failed extraction or run fails the suite, as does a results file with fewer
# rows than the list: an example that never ran is not one that still passes.
# Unchecked, an extractor crash left most examples unextracted, run.sh bailed
# out, and the missing results file counted as zero regressions.
rubyspec-gate: $(SPINEL) $(RUBYSPEC_DIR)/.pinned
	@ok=1; for d in $(RUBYSPEC_SUITES); do \
	  nm=$$(echo $$d | tr / -); \
	  rm -rf build/rubyspec-ex-$$nm build/rubyspec-gate-$$nm.tsv; \
	  if ! ruby tools/rubyspec/extract.rb $(RUBYSPEC_DIR)/$$d build/rubyspec-ex-$$nm; then \
	    echo "rubyspec-gate[$$d]: extraction failed"; ok=0; continue; \
	  fi; \
	  awk -F'\t' '$$2=="PASS"{print $$1}' tools/rubyspec/expectations/$$nm.tsv > build/rubyspec-gate-$$nm.list; \
	  if ! RUBYSPEC_ONLY=build/rubyspec-gate-$$nm.list RUBYSPEC_GATE=1 \
	    bash tools/rubyspec/run.sh build/rubyspec-ex-$$nm build/rubyspec-gate-$$nm.tsv >/dev/null; then \
	    echo "rubyspec-gate[$$d]: run.sh failed"; ok=0; continue; \
	  fi; \
	  want=$$(wc -l < build/rubyspec-gate-$$nm.list); \
	  ran=$$(wc -l < build/rubyspec-gate-$$nm.tsv); \
	  bad=$$(awk -F'\t' '$$2!="PASS"' build/rubyspec-gate-$$nm.tsv | wc -l); \
	  if [ $$ran -ne $$want ]; then \
	    echo "rubyspec-gate[$$d]: ran $$ran of $$want expected-PASS examples"; ok=0; \
	  elif [ $$bad -ne 0 ]; then \
	    echo "rubyspec-gate[$$d]: $$bad regression(s):"; \
	    awk -F'\t' '$$2!="PASS"' build/rubyspec-gate-$$nm.tsv; ok=0; \
	  else \
	    echo "rubyspec-gate[$$d]: all $$(wc -l < build/rubyspec-gate-$$nm.list) expected-PASS examples still pass"; \
	  fi; \
	done; [ $$ok -eq 1 ]

# ---- Optcarrot integration test ----
# Forcing the small leaf methods inline is the default now; keep the knob so a
# before-and-after can measure the plain build, which is what an inference
# change wants when the question is whether the emitted code changed rather
# than how the inliner reacted: `make optcarrot OPTCARROT_FLAGS=--no-inline-hot`.
OPTCARROT_FLAGS ?=
OPTCARROT_DIR  := build/optcarrot
OPTCARROT_REPO := https://github.com/mame/optcarrot.git
OPTCARROT_BRANCH := experiment/spinel

optcarrot: $(SPINEL) $(SP_RT_LIB) $(SPINEL_TIMEOUT)
	@if [ ! -d $(OPTCARROT_DIR) ]; then \
	  git clone --depth=1 --branch=$(OPTCARROT_BRANCH) $(OPTCARROT_REPO) $(OPTCARROT_DIR); \
	fi
	@ruby $(OPTCARROT_DIR)/tools/pack-for-spinel.rb > build/optcarrot-single.rb
	@$(SPINEL) $(OPTCARROT_FLAGS) build/optcarrot-single.rb -c --no-line-map -o build/optcarrot-single.c
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
	  echo "Optcarrot: FAIL -- expected 'fps: <num>' and 'checksum: 59662'"; \
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
# the 800+ unreferenced static fns before codegen). Skips bench/optcarrot --
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
#
# Every fixture is also BUILT and RUN first. TESTS is `test/*.rb`, which does
# not reach this directory, and the assertions below read the emitted C as text
# only -- so without this a fixture whose C names the right types but does not
# compile, or compiles and then raises, passes the whole leg.
infer-test: $(SPINEL) $(SP_RT_LIB)
	@tmp=$$(mktemp -d /tmp/spinel-infer.XXXXXX); ok=1; \
	for f in test/infer/*.rb; do \
	  $(SPINEL) "$$f" -o "$$tmp/ibin" >/dev/null 2>&1 || { echo "infer-test: FAIL ($$f: the emitted C does not compile)"; ok=0; continue; }; \
	  "$$tmp/ibin" >/dev/null 2>&1 || { echo "infer-test: FAIL ($$f: the program does not run)"; ok=0; }; \
	done; \
	$(SPINEL) test/infer/hash_one_class_each_value.rb -c --no-line-map -o "$$tmp/hoc.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (hash_one_class_each_value: -c)"; ok=0; }; \
	grep -q 'sp_Item \* lv_it' "$$tmp/hoc.c" && grep -q 'sp_Item_describe((sp_Item \*)lv_it)' "$$tmp/hoc.c" || { echo "infer-test: FAIL (#4846 a one-class hash's each_value is not typed)"; ok=0; }; \
	$(SPINEL) test/infer/hash_or_write_index_setter.rb -c --no-line-map -o "$$tmp/hos.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (hash_or_write_index_setter: -c)"; ok=0; }; \
	grep -q 'sp_PolyPolyHash \* iv_traps;' "$$tmp/hos.c" && grep -q 'sp_PolyPolyHash \* iv_hooks;' "$$tmp/hos.c" || { echo "infer-test: FAIL (#4889 an index write into (@h ||= {}) left @h boxed)"; ok=0; }; \
	grep -q 'sp_OrwMem_poke(sp_OrwMem \*self, sp_int lv_addr, sp_int lv_value)' "$$tmp/hos.c" || { echo "infer-test: FAIL (#4889 a Hash index write widened an unrelated user []=)"; ok=0; }; \
	SPINEL_SPLIT_STRICT=1 $(SPINEL) --jobs=3 test/dispatch_override_param_list.rb -o "$$tmp/split" >/dev/null 2>&1 && "$$tmp/split" | cmp -s - test/dispatch_override_param_list.rb.expected || { echo "infer-test: FAIL (#4847 --jobs=3 split build)"; ok=0; }; \
	$(SPINEL) test/infer/file_foreach_block_streams.rb -c --no-line-map -o "$$tmp/ffbs.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (file_foreach_block_streams: -c)"; ok=0; }; \
	grep -q 'sp_file_readlines(' "$$tmp/ffbs.c" && { echo "infer-test: FAIL (File.foreach with a block reads the whole file through readlines)"; ok=0; }; \
	$(SPINEL) test/io_each_block_param_typed.rb -c --no-line-map -o "$$tmp/iebp.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (io_each_block_param_typed: -c)"; ok=0; }; \
	for v in lines viaeach chomped chars; do grep -q "sp_StrArray \* lv_$$v = " "$$tmp/iebp.c" || { echo "infer-test: FAIL (an array a File's each_line/each/each_char block pushes into is not a String array: $$v)"; ok=0; }; done; \
	for v in bytes cps; do grep -q "sp_IntArray \* lv_$$v = " "$$tmp/iebp.c" || { echo "infer-test: FAIL (an array a File's each_byte/each_codepoint block pushes into is not an Integer array: $$v)"; ok=0; }; done; \
	$(SPINEL) test/io_buffer_set_value_boxed.rb -c --no-line-map -o "$$tmp/iob.c" >/dev/null 2>&1 && $(CC) -fsyntax-only -Werror=implicit-function-declaration -Ilib "$$tmp/iob.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (the emitted C calls an IO::Buffer function it does not declare)"; ok=0; }; \
	$(SPINEL) test/poly_array_break_no_setjmp.rb -c --no-line-map -o "$$tmp/pab.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (poly_array_break_no_setjmp: -c)"; ok=0; }; \
	awk '/^static .*sp_Board_[a-z_]*\(.*\) \{$$/ {b=1} b {print} b && /^}/ {b=0}' "$$tmp/pab.c" > "$$tmp/pab_board.c"; \
	[ -s "$$tmp/pab_board.c" ] && ! grep -q 'sp_brk_push' "$$tmp/pab_board.c" || { echo "infer-test: FAIL (#4916 a break out of a walk over an object array pays a setjmp)"; ok=0; }; \
	$(SPINEL) test/infer/poly_dispatch_out_of_line.rb -c --no-line-map -o "$$tmp/pdl.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (poly_dispatch_out_of_line: -c)"; ok=0; }; \
	[ "$$(grep -c '^static .*sp_pd_[0-9]*(sp_RbVal _t0) {' "$$tmp/pdl.c")" = 1 ] && [ "$$(grep -o '= sp_pd_[0-9]*(' "$$tmp/pdl.c" | wc -l | tr -d ' ')" = 2 ] || { echo "infer-test: FAIL (#4847 a poly dispatch is not one shared out-of-line function)"; ok=0; }; \
	$(SPINEL) test/infer/poly_dispatch_out_of_line.rb -c -o "$$tmp/pdl_lm.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (poly_dispatch_out_of_line: -c with line map)"; ok=0; }; \
	awk '/^#line /{seen=1; d=1; next} seen && !d && !c && !/^#/ {bad++} {d=0; c=/\\$$/} END{exit bad>0}' "$$tmp/pdl_lm.c" || { echo "infer-test: FAIL (#4940 a C line after a statement's first is not re-anchored to its Ruby line)"; ok=0; }; \
	grep -B1 '^static .*sp_pd_[0-9]*(sp_RbVal _t0) {' "$$tmp/pdl_lm.c" | grep -q '^#line 12 "test/infer/poly_dispatch_out_of_line.rb"' || { echo "infer-test: FAIL (#4928 an out-of-line dispatch function does not name the call site it came from)"; ok=0; }; \
	$(SPINEL) test/infer/string_handle_proc_reader.rb -c --no-line-map -o "$$tmp/shr.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (string_handle_proc_reader: -c)"; ok=0; }; \
	grep -q 'const char \* lv_r = (argc > 0) ? (const char \*)(uintptr_t)args\[0\]' "$$tmp/shr.c" && grep -q 'const char \* lv_line = NULL;' "$$tmp/shr.c" && grep -q 'sp_String \* lv_buf = NULL;' "$$tmp/shr.c" || { echo "infer-test: FAIL (#6179 a proc that only reads its String took the shared handle)"; ok=0; }; \
	$(SPINEL) test/infer/string_handle_initialize_reader.rb -c --no-line-map -o "$$tmp/shi.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (string_handle_initialize_reader: -c)"; ok=0; }; \
	grep -q 'sp_Count_initialize(sp_Count \*self, const char \* lv_s)' "$$tmp/shi.c" && grep -q 'sp_Name_initialize(sp_Name \*self, const char \* lv_s)' "$$tmp/shi.c" && grep -q 'const char \* lv_line = NULL;' "$$tmp/shi.c" && grep -q 'sp_Grow_initialize(sp_Grow \*self, sp_String \* lv_s)' "$$tmp/shi.c" || { echo "infer-test: FAIL (#6179 an initialize that only reads or keeps its String took the shared handle)"; ok=0; }; \
	$(SPINEL) test/infer/tally_typed.rb -c --no-line-map -o "$$tmp/tly.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (tally_typed: -c)"; ok=0; }; \
	grep -q '^static inline sp_IntIntHash \* sp___enum_tally__[0-9]*(' "$$tmp/tly.c" && grep -q '^static inline sp_StrIntHash \* sp___enum_tally__[0-9]*(' "$$tmp/tly.c" || { echo "infer-test: FAIL (tally over a typed array answers a boxed hash)"; ok=0; }; \
	$(SPINEL) test/infer/param_narrow_super_route.rb -c --no-line-map -o "$$tmp/psr.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (param_narrow_super_route: -c)"; ok=0; }; \
	grep -q 'sp_Holder_initialize(sp_Holder \*self, sp_RbVal lv_v)' "$$tmp/psr.c" || { echo "infer-test: FAIL (a parameter reached by super was narrowed from the visible calls alone)"; ok=0; }; \
	grep -q 'sp_Plain_initialize(sp_Plain \*self, sp_int lv_v)' "$$tmp/psr.c" || { echo "infer-test: FAIL (the super guard stopped an unrelated parameter narrowing)"; ok=0; }; \
	$(SPINEL) test/infer/ivar_typed_array_meets_boxed_array.rb -c --no-line-map -o "$$tmp/tmb.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (ivar_typed_array_meets_boxed_array: -c)"; ok=0; }; \
	grep -q 'sp_PolyArray \* iv_xs;' "$$tmp/tmb.c" || { echo "infer-test: FAIL (#5521 a slot holding only Arrays widened to a boxed value)"; ok=0; }; \
	grep -q 'sp_RbVal iv_ys;' "$$tmp/tmb.c" || { echo "infer-test: FAIL (#4196 two typed array kinds no longer box)"; ok=0; }; \
	[ "$$(grep -c 'sp_PolyArray \* iv_items;' "$$tmp/tmb.c")" = 3 ] || { echo "infer-test: FAIL (#5521 a transplanted copy of a module's array ivar widened to a boxed value)"; ok=0; }; \
	$(SPINEL) test/reader_read_only_no_copy.rb -c --no-line-map -o "$$tmp/rro.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (reader_read_only_no_copy: -c)"; ok=0; }; \
	grep -q '? sp_String_cstr(_t' "$$tmp/rro.c" || { echo "infer-test: FAIL (a read-only reader read still copies the whole String)"; ok=0; }; \
	[ "$$(grep -c 'sp_str_concat(sp_String_cstr' "$$tmp/rro.c")" -le 2 ] || { echo "infer-test: FAIL (a read-only reader read copies where it need not)"; ok=0; }; \
	$(SPINEL) test/setbyte_handle_in_place.rb -c --no-line-map -o "$$tmp/sbh.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (setbyte_handle_in_place: -c)"; ok=0; }; \
	grep -q 'sp_str_setbyte_cow(_p' "$$tmp/sbh.c" || { echo "infer-test: FAIL (setbyte on a handle still reads the whole String out)"; ok=0; }; \
	awk '/while \\(\\(lv_i < 16LL\\)\\)/{f=1} f{print} f&&/^  }/{exit}' "$$tmp/sbh.c" | grep -q 'sp_str_concat' && { echo "infer-test: FAIL (a setbyte loop copies the String per write)"; ok=0; }; \
	$(SPINEL) test/renarrow_resets_return.rb -c --no-line-map -o "$$tmp/rrr.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (renarrow_resets_return: -c)"; ok=0; }; \
	grep -q 'sp_int sp_Rng_next_u32(' "$$tmp/rrr.c" || { echo "infer-test: FAIL (a self-referential ivar through a return stayed boxed)"; ok=0; }; \
	grep -q 'sp_float sp_Rng_uniform(' "$$tmp/rrr.c" || { echo "infer-test: FAIL (the Float built from it stayed boxed)"; ok=0; }; \
	$(SPINEL) test/index_opassign_fused.rb -c --no-line-map -o "$$tmp/iof.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (index_opassign_fused: -c)"; ok=0; }; \
	grep -qE 'sp_IntArray \* _t[0-9]+ = lv_counts; .*(->frozen|_hcw[0-9_]+) && \(unsigned long long\)' "$$tmp/iof.c" || { echo "infer-test: FAIL (an Integer slot's op-assign still reads and writes through two bounds checks)"; ok=0; }; \
	grep -qE 'sp_FloatArray \* _t[0-9]+ = lv_zsum; .*(->frozen|_hcw[0-9_]+) && \(unsigned long long\)' "$$tmp/iof.c" || { echo "infer-test: FAIL (a Float slot's op-assign with a typed-array RHS is not folded in place)"; ok=0; }; \
	grep -E 'sp_IntArray \* _t[0-9]+ = lv_g;' "$$tmp/iof.c" | grep -q -- '->frozen &&' && { echo "infer-test: FAIL (an op-assign whose RHS runs code was folded through an element pointer)"; ok=0; }; \
	grep -qE 'sp_IntArray \* _t([0-9]+) = self->iv_a; SP_GC_ROOT\(_t\1\)' "$$tmp/iof.c" || { echo "infer-test: FAIL (an op-assign's receiver is unrooted while a key that can reassign it runs)"; ok=0; }; \
	$(SPINEL) test/loop_array_header_cache.rb -c --no-line-map -o "$$tmp/lahc.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (loop_array_header_cache: -c)"; ok=0; }; \
	grep -qE '^#define _SP_HCR[0-9]+\(\) .*lv_bins.*lv_counts.*lv_vals.*lv_sums' "$$tmp/lahc.c" || { echo "infer-test: FAIL (a loop that only indexes typed arrays still reads their headers at every access)"; ok=0; }; \
	grep -E '^#define _SP_HCR' "$$tmp/lahc.c" | grep -q 'lv_cur\b' && { echo "infer-test: FAIL (an array local the loop reassigns was read through a cached header)"; ok=0; }; \
	grep -E '^#define _SP_HCR' "$$tmp/lahc.c" | grep -q 'self->iv_v\b' && { echo "infer-test: FAIL (an ivar the loop writes was read through a cached header)"; ok=0; }; \
	grep -qE '^#define _SP_HCR[0-9]+\(\) .*lv_qv.*lv_qk.*lv_qs' "$$tmp/lahc.c" || { echo "infer-test: FAIL (a class test on a scalar kept a loop from caching its arrays' headers)"; ok=0; }; \
	$(SPINEL) test/loop_bounded_index_read.rb -c --no-line-map -o "$$tmp/lbi.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (loop_bounded_index_read: -c)"; ok=0; }; \
	grep -qE '_hcd[0-9]+_[0-9]+\[lv_i\]' "$$tmp/lbi.c" && grep -qE '_hcd[0-9]+_[0-9]+\[lv_j\]' "$$tmp/lbi.c" || { echo "infer-test: FAIL (a read bounded by its loop's own i < a.length test still tests its index)"; ok=0; }; \
	grep -qE '_hcd[0-9]+_[0-9]+\[lv_(m|q|r|w|x|y|z)\]' "$$tmp/lbi.c" && { echo "infer-test: FAIL (a read whose index the loop does not keep in range lost its bounds test)"; ok=0; }; \
	grep -qE 'while \(\(lv_i < _hcl[0-9]+_[0-9]+\)\)' "$$tmp/lbi.c" || { echo "infer-test: FAIL (a loop that reads a[i] untested does not test i against the cached length)"; ok=0; }; \
	grep -qE 'lv_(m|q|r|w|y|z) < _hcl' "$$tmp/lbi.c" && { echo "infer-test: FAIL (a loop whose index is not kept in range tests it against the cached length)"; ok=0; }; \
	$(SPINEL) test/loop_bounded_index_polls.rb -c --no-line-map -o "$$tmp/lbp.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (loop_bounded_index_polls: -c)"; ok=0; }; \
	grep -F 'while (({ if (SP_UNLIKELY(SP_SAFEPOINT_POLL())) sp_safepoint(), _SP_HCR' "$$tmp/lbp.c" | grep -qF 'sp_fin_run_pending(), _SP_HCR' && grep -qE '_hcd[0-9]+_[0-9]+\[lv_i\]' "$$tmp/lbp.c" || { echo "infer-test: FAIL (a loop that reads a[i] untested does not poll ahead of its i < a.length test)"; ok=0; }; \
	awk '/^[a-z].* sp_total\(.*\{$$/,/^}/' "$$tmp/lbp.c" | awk 'f { print; exit } /while \(\(\{/ { f = 1 }' | grep -qE 'SP_SAFEPOINT_POLL|SP_FIN_POLL|sp_fin_run_pending' && { echo "infer-test: FAIL (a loop that reads a[i] untested polls between its test and the read)"; ok=0; }; \
	awk '/^[a-z].* sp_back\(.*\{$$/,/^}/' "$$tmp/lbp.c" | grep -qE '^ +if .*sp_fin_run_pending\(\), _SP_HCR' || { echo "infer-test: FAIL (a cached loop does not read its headers again after a finalizer runs)"; ok=0; }; \
	$(SPINEL) test/array_new_fill_sized.rb -c --no-line-map -o "$$tmp/anf.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (array_new_fill_sized: -c)"; ok=0; }; \
	grep -q 'sp_IntArray_new_fill(' "$$tmp/anf.c" && grep -q 'sp_FloatArray_new_fill(' "$$tmp/anf.c" || { echo "infer-test: FAIL (Array.new(n, v) on an Integer or Float array grows by n pushes instead of allocating n)"; ok=0; }; \
	$(SPINEL) test/reader_operands_pure_read.rb -c --no-line-map -o "$$tmp/rop.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (reader_operands_pure_read: -c)"; ok=0; }; \
	awk '/^[a-z].* sp_total\(/,/^}/' "$$tmp/rop.c" | grep -q 'SP_GC_ROOT(_t' && { echo "infer-test: FAIL (operands that are all pure reads were bound to rooted temps)"; ok=0; }; \
	grep -qE 'sp_IntArray \* _t[0-9]+ = sp_Loud_vals\(\(sp_Loud \*\)lv_l\); SP_GC_ROOT' "$$tmp/rop.c" || { echo "infer-test: FAIL (a def overriding a reader was taken for a pure field read)"; ok=0; }; \
	grep -qE '= \(lv_h\)->iv_data; SP_GC_ROOT\(_t' "$$tmp/rop.c" || { echo "infer-test: FAIL (a reader next to a call that reassigns it lost its ordering)"; ok=0; }; \
	$(SPINEL) test/infer/typed_array_elem_arg_types_param.rb -c --no-line-map -o "$$tmp/tae.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (typed_array_elem_arg_types_param: -c)"; ok=0; }; \
	grep -q 'sp_Plan_write_column(sp_Plan \*self, sp_int lv_wcol)' "$$tmp/tae.c" || { echo "infer-test: FAIL (an int-array element passed as an argument left the parameter boxed)"; ok=0; }; \
	grep -q 'sp_Plan_shout(sp_Plan \*self, const char \* lv_s)' "$$tmp/tae.c" || { echo "infer-test: FAIL (a String-array element passed as an argument left the parameter boxed)"; ok=0; }; \
	grep -q 'sp_Mixed_take(sp_Mixed \*self, sp_RbVal lv_v)' "$$tmp/tae.c" || { echo "infer-test: FAIL (a parameter whose call sites pass two element kinds must keep the boxed slot)"; ok=0; }; \
	$(SPINEL) test/infer/array_new_default_push_narrows.rb -c --no-line-map -o "$$tmp/and.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (array_new_default_push_narrows: -c)"; ok=0; }; \
	grep -q 'sp_FloatArray \* iv_f;' "$$tmp/and.c" && grep -q 'sp_StrArray \* iv_s;' "$$tmp/and.c" || { echo "infer-test: FAIL (an Array.new(n, default) slot pushed a parameter stayed boxed)"; ok=0; }; \
	grep -q 'sp_PolyArray \* iv_m;' "$$tmp/and.c" || { echo "infer-test: FAIL (a slot whose pushes disagree must stay boxed)"; ok=0; }; \
	grep -q 'sp_PolyArray \* iv_banks;' "$$tmp/and.c" || { echo "infer-test: FAIL (a table stored a boxed row must stay boxed)"; ok=0; }; \
	$(SPINEL) test/infer/object_array_map.rb -c --no-line-map -o "$$tmp/oam.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (object_array_map: -c)"; ok=0; }; \
	grep -q 'sp_PtrArray \* iv_list;' "$$tmp/oam.c" || { echo "infer-test: FAIL (#4846 an array of one class walked by map stayed boxed)"; ok=0; }; \
	grep -q '(lv_x)->iv_name' "$$tmp/oam.c" || { echo "infer-test: FAIL (#4846 an element call is not a direct read)"; ok=0; }; \
	$(SPINEL) test/infer/struct_cmethod_bare_new.rb -c --no-line-map -o "$$tmp/sn.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (struct_cmethod_bare_new: -c)"; ok=0; }; \
	for m in 'sp_sym iv_op;' 'sp_int iv_n;' 'sp_int iv_a;' 'const char \* iv_b;' 'sp_int iv_v;'; do \
	  grep -q "$$m" "$$tmp/sn.c" || { echo "infer-test: FAIL (struct_cmethod_bare_new: member not typed: $$m)"; ok=0; }; \
	done; \
	$(SPINEL) test/infer/emit_types_fields.rb --emit-types -o "$$tmp/et.json" >/dev/null 2>&1 || { echo "infer-test: FAIL (--emit-types on emit_types_fields)"; exit 1; }; \
	grep -q '"line":13,"col":5,"end_line":13,"end_col":8,"kind":"LocalVariableReadNode","name":"pts"' "$$tmp/et.json" || { echo "infer-test: FAIL (--emit-types: a node's span, kind and name)"; ok=0; }; \
	grep -q '"line":13,"col":5,"end_line":13,"end_col":36,"kind":"CallNode","name":"map"' "$$tmp/et.json" || { echo "infer-test: FAIL (--emit-types: the enclosing call span)"; ok=0; }; \
	grep -q '"line":6,"col":12,.*"method":"dist2","slot":"param","param":"o"' "$$tmp/et.json" || { echo "infer-test: FAIL (--emit-types: the widened parameter is named and placed)"; ok=0; }; \
	grep -q '"method":"widen","slot":"return"' "$$tmp/et.json" || { echo "infer-test: FAIL (--emit-types: the widened return is named)"; ok=0; }; \
	grep -q '"line":13,"col":19,.*"kind":"CallNode","name":"dist2","dispatch":"switch"' "$$tmp/et.json" || { echo "infer-test: FAIL (--emit-types: a class switch is reported as one)"; ok=0; }; \
	grep -q '"line":13,"col":13,.*"kind":"BlockNode","inlined":true' "$$tmp/et.json" || { echo "infer-test: FAIL (--emit-types: an inlined block is reported as one)"; ok=0; }; \
	grep -q '"line":16,"col":4,.*"kind":"CallNode","name":"widen","dispatch":"direct"' "$$tmp/et.json" || { echo "infer-test: FAIL (--emit-types: a direct call is reported as one)"; ok=0; }; \
	grep -q '"line":6,"col":12,"end_line":6,"end_col":13,"kind":"RequiredParameterNode","name":"o","type":"poly","rbs":"untyped"' "$$tmp/et.json" || { echo "infer-test: FAIL (--emit-types: a parameter has a record carrying the slot type)"; ok=0; }; \
	grep -q '"line":4,"col":17,.*"kind":"RequiredParameterNode","name":"x","type":"int","rbs":"Integer"' "$$tmp/et.json" || { echo "infer-test: FAIL (--emit-types: a typed parameter record)"; ok=0; }; \
	grep -q '"line":16,"col":4,.*"kind":"CallNode","name":"widen","dispatch":"direct","callee":"widen"' "$$tmp/et.json" || { echo "infer-test: FAIL (--emit-types: a direct call names its callee)"; ok=0; }; \
	grep -q '"line":13,"col":19,.*"kind":"CallNode","name":"dist2","dispatch":"switch","candidates":\["Point#dist2"\]' "$$tmp/et.json" || { echo "infer-test: FAIL (--emit-types: a switch lists its candidate defs)"; ok=0; }; \
	grep -q '"kind":"DefNode","name":"dist2",.*"owner":"Point","signature":"(untyped) -> Integer","widened":true' "$$tmp/et.json" || { echo "infer-test: FAIL (--emit-types: a def carries its owner and signature)"; ok=0; }; \
	grep -q '"kind":"DefNode","name":"widen",.*"owner":"Object","signature":"(untyped, Integer) -> untyped","widened":true' "$$tmp/et.json" || { echo "infer-test: FAIL (--emit-types: a top-level def is owned by Object)"; ok=0; }; \
	grep -q '"kind":"DefNode","name":"x",.*"signature":"() -> Integer"}' "$$tmp/et.json" || { echo "infer-test: FAIL (--emit-types: an unwidened def carries no widened flag)"; ok=0; }; \
	$(SPINEL) test/infer/emit_types_fields.rb --emit-types -o "$$tmp/et2.json" -S > "$$tmp/et2.c" 2>/dev/null || { echo "infer-test: FAIL (--emit-types -S)"; ok=0; }; \
	cmp -s "$$tmp/et.json" "$$tmp/et2.json" && grep -q 'sp_Point_dist2' "$$tmp/et2.c" || { echo "infer-test: FAIL (--emit-types -S: the same JSON and the C in one run)"; ok=0; }; \
	$(SPINEL) test/infer/emit_types_fields.rb -S > "$$tmp/et3.c" 2>/dev/null && cmp -s "$$tmp/et2.c" "$$tmp/et3.c" || { echo "infer-test: FAIL (--emit-types -S: the C is the one -S alone emits, not the debug compile's)"; ok=0; }; \
	$(SPINEL) test/infer/emit_types_fields.rb --no-line-map --emit-types -o "$$tmp/et4.json" >/dev/null 2>&1 && cmp -s "$$tmp/et.json" "$$tmp/et4.json" || { echo "infer-test: FAIL (--emit-types: positions survive --no-line-map)"; ok=0; }; \
	printf 'def helper(x)\n  x +\nend\n' > "$$tmp/pe_lib.rb"; printf 'require_relative "pe_lib"\nputs helper(1)\n' > "$$tmp/pe.rb"; \
	(cd "$$tmp" && $(CURDIR)/$(SPINEL) pe.rb --emit-types -o pe.json 2> pe.err; test $$? -eq 1) || { echo "infer-test: FAIL (a parse error still exits 1)"; ok=0; }; \
	grep -q '^  pe_lib.rb:3:1: unexpected' "$$tmp/pe.err" || { echo "infer-test: FAIL (a parse error is placed, file:line:col, through the require map)"; cat "$$tmp/pe.err"; ok=0; }; \
	grep -q '"types": \[' "$$tmp/pe.json" && grep -q '{"file":"pe_lib.rb","line":3,"col":0,"end_line":3,"end_col":3,"severity":"error","message":"unexpected' "$$tmp/pe.json" || { echo "infer-test: FAIL (--emit-types on a parse error writes the errors as diagnostics)"; ok=0; }; \
	$(SPINEL) test/infer/emit_types_fields.rb --warn-widen -c -o "$$tmp/ww.c" 2> "$$tmp/ww.err" >/dev/null || { echo "infer-test: FAIL (--warn-widen compiles)"; ok=0; }; \
	test "$$(grep -c ': warning: ' "$$tmp/ww.err")" = 3 && grep -q '^spinel: test/infer/emit_types_fields.rb:6:13: warning: parameter `o` of `dist2` widened to untyped' "$$tmp/ww.err" && grep -q '^spinel: test/infer/emit_types_fields.rb:11:1: warning: the return of `widen` widened' "$$tmp/ww.err" || { echo "infer-test: FAIL (--warn-widen: one warning per widened slot, at the slot, 1-based column)"; cat "$$tmp/ww.err"; ok=0; }; \
	$(SPINEL) test/infer/emit_types_fields.rb -c -o "$$tmp/nw.c" 2>&1 >/dev/null | grep -q ': warning: ' && { echo "infer-test: FAIL (a plain compile stays quiet about widening)"; ok=0; }; \
	$(SPINEL) test/infer/emit_types_fields.rb --warn-widen --no-line-map -c -o "$$tmp/ww2.c" 2>&1 >/dev/null | grep -q '^spinel: test/infer/emit_types_fields.rb:6:13: warning' || { echo "infer-test: FAIL (--warn-widen places past --no-line-map)"; ok=0; }; \
	$(SPINEL) test/infer/emit_types_fields.rb --warn-widen --emit-rbs -o "$$tmp/ww.rbs" 2>&1 >/dev/null | grep -q ': warning: parameter `o` of `dist2`' || { echo "infer-test: FAIL (--warn-widen warns under --emit-rbs too)"; ok=0; }; \
	grep -q '^spinel: test/infer/emit_types_fields.rb:13:28: note: passed `pts\[0\]` is untyped' "$$tmp/ww.err" && grep -q '^spinel: test/infer/emit_types_fields.rb:12:7: note: from `\[Point.new(1, 2), Point.new(3, 4)\]` is Array\[untyped\] -- born here' "$$tmp/ww.err" || { echo "infer-test: FAIL (--warn-widen: why: an inherited poly is followed to the expression it was born at)"; cat "$$tmp/ww.err"; ok=0; }; \
	grep -q '^spinel: test/infer/emit_types_fields.rb:15:7: note: passed `"s"` is String, where the slot was Integer (two kinds meet: untyped)' "$$tmp/ww.err" && grep -q '^spinel: test/infer/emit_types_fields.rb:14:7: note: and `1` is Integer' "$$tmp/ww.err" || { echo "infer-test: FAIL (--warn-widen: why: two kinds meeting name both call sites)"; cat "$$tmp/ww.err"; ok=0; }; \
	grep -q '"param":"a",.*"why":\[{"file":"test/infer/emit_types_fields.rb","line":15,"col":6,"end_line":15,"end_col":9,"role":"passed","rbs":"String","note":", where the slot was Integer (two kinds meet: untyped)"},{"file":"test/infer/emit_types_fields.rb","line":14,"col":6,"end_line":14,"end_col":7,"role":"and","rbs":"Integer"}\]' "$$tmp/et.json" || { echo "infer-test: FAIL (--emit-types: the widening diagnostic carries its why)"; ok=0; }; \
	$(SPINEL) test/infer/why_widen_chain.rb --warn-widen -c -o "$$tmp/wwc.c" 2> "$$tmp/wwc.err" >/dev/null || { echo "infer-test: FAIL (compile why_widen_chain)"; cat "$$tmp/wwc.err"; exit 1; }; \
	grep -q '^spinel: test/infer/why_widen_chain.rb:10:16: note: returned `v.to_s` is untyped (a candidate of the send, `Bad#to_s`, returns untyped)' "$$tmp/wwc.err" && grep -q '^spinel: test/infer/why_widen_chain.rb:7:14: note: returned `@v` is untyped' "$$tmp/wwc.err" || { echo "infer-test: FAIL (--warn-widen: why: a send on a poly receiver follows the candidate whose return degraded, not the receiver)"; cat "$$tmp/wwc.err"; ok=0; }; \
	grep -q '^spinel: test/infer/why_widen_chain.rb:15:13: note: written `\[\]` is untyped (no type of its own) (no type of its own: an empty literal); the Foo pushed into it makes an Array of objects, which has no typed form: Array\[untyped\] by representation' "$$tmp/wwc.err" || { echo "infer-test: FAIL (--warn-widen: why: an empty literal filled with objects is untyped by representation, not by the round)"; cat "$$tmp/wwc.err"; ok=0; }; \
	grep -q '^spinel: test/infer/why_widen_chain.rb:26:3: note: returned `return nil if n > 5…` is Symbol, where a `return` gives nil (two kinds meet: untyped)' "$$tmp/wwc.err" && grep -q '^spinel: test/infer/why_widen_chain.rb:26:10: note: and `nil` is nil' "$$tmp/wwc.err" || { echo "infer-test: FAIL (--warn-widen: why: a return that met two kinds names the return of the other kind)"; cat "$$tmp/wwc.err"; ok=0; }; \
	$(SPINEL) test/infer/why_widen_chain.rb -c --no-line-map -o "$$tmp/wwc2.c" 2> "$$tmp/wwc2.err" >/dev/null || { echo "infer-test: FAIL (plain compile why_widen_chain)"; cat "$$tmp/wwc2.err"; ok=0; }; \
	$(SPINEL) test/infer/why_widen_rules.rb --warn-widen --emit-types -o "$$tmp/wwr.json" 2> "$$tmp/wwr.err" >/dev/null || { echo "infer-test: FAIL (compile why_widen_rules)"; cat "$$tmp/wwr.err"; exit 1; }; \
	grep -q '^spinel: test/infer/why_widen_rules.rb:5:16: note: by `nil` is nil -- a `= nil` default and no call site typing it' "$$tmp/wwr.err" && grep -q '^spinel: test/infer/why_widen_rules.rb:6:13: note: by `{}` is untyped (no type of its own) -- an empty literal default' "$$tmp/wwr.err" || { echo "infer-test: FAIL (--warn-widen: a rule that widened a parameter says so in its words, at its subject)"; cat "$$tmp/wwr.err"; ok=0; }; \
	grep -q '^spinel: note: by construction: a splat parameter' "$$tmp/wwr.err" && grep -q '^spinel: note: never bound: no call site gives it a type' "$$tmp/wwr.err" || { echo "infer-test: FAIL (--warn-widen: a splat and a never-bound parameter say so)"; cat "$$tmp/wwr.err"; ok=0; }; \
	grep -q '^spinel: test/infer/why_widen_rules.rb:5:36: note: from `b` is untyped' "$$tmp/wwr.err" && test "$$(grep -c 'a `= nil` default and no call site' "$$tmp/wwr.err")" = 2 || { echo "infer-test: FAIL (--warn-widen: a chain reaching a rule-widened parameter ends in its words)"; cat "$$tmp/wwr.err"; ok=0; }; \
	grep -q '"param":"xs",.*"why":\[{"role":"rule","note":"by construction: a splat parameter holds the extra arguments of every call, untyped"}\]' "$$tmp/wwr.json" && grep -q '"param":"b",.*"why":\[{"file":"test/infer/why_widen_rules.rb","line":5,"col":15,"end_line":5,"end_col":18,"role":"by","rbs":"nil","note":" -- a `= nil` default' "$$tmp/wwr.json" || { echo "infer-test: FAIL (--emit-types: a rule is a why hop, with its subject when it has one)"; ok=0; }; \
	grep -q ': note: ' "$$tmp/wwc2.err" && { echo "infer-test: FAIL (a plain compile derives no origin)"; ok=0; }; \
	$(SPINEL) test/infer/class_of_known_receiver_reach.rb -c --no-line-map -o "$$tmp/ckr.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile class_of_known_receiver_reach)"; exit 1; }; \
	grep -Eq 'sp_Digest_initialize\(sp_Digest \*self, const char \* lv_raw_hash\)' "$$tmp/ckr.c" || { echo "infer-test: FAIL (a handed-on .class of a known receiver let a class-value new reach every class)"; grep -E 'sp_Digest_initialize\(' "$$tmp/ckr.c" | head -1; ok=0; }; \
	$(SPINEL) test/infer/const_get_literal_reach.rb -c --no-line-map -o "$$tmp/cgl.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile const_get_literal_reach)"; exit 1; }; \
	grep -Eq 'sp_Digest_initialize\(sp_Digest \*self, const char \* lv_raw_hash\)' "$$tmp/cgl.c" || { echo "infer-test: FAIL (a literal const_get let a class-value new reach every class's initialize)"; grep -E 'sp_Digest_initialize\(' "$$tmp/cgl.c" | head -1; ok=0; }; \
	$(SPINEL) test/class_method_self_is_no_escape.rb -c --no-line-map -o "$$tmp/cms.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile class_method_self_is_no_escape)"; exit 1; }; \
	grep -Eq 'sp_Digest_initialize\(sp_Digest \*self, const char \* lv_raw_hash\)' "$$tmp/cms.c" || { echo "infer-test: FAIL (the self of def self.m let a class-value new reach every class with a class method)"; grep -E 'sp_Digest_initialize\(' "$$tmp/cms.c" | head -1; ok=0; }; \
	$(SPINEL) test/infer/unsettled_index_write.rb -c --no-line-map -o "$$tmp/u.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile unsettled_index_write)"; exit 1; }; \
	grep -Eq 'static (inline )?((__attribute__\(\(always_inline\)\)|SP_ALWAYS_INLINE) )?sp_int sp_M_s_mul\(sp_int [A-Za-z_]+, sp_int [A-Za-z_]+\)' "$$tmp/u.c" || { echo "infer-test: FAIL (an int-keyed []= on an unsettled slot poisoned the call graph)"; grep -E 'sp_M_s_mul\(' "$$tmp/u.c" | head -1; ok=0; }; \
	grep -Eq 'sp_IntArray \* *lv_xs' "$$tmp/u.c" || { echo "infer-test: FAIL (the mapped array did not settle to an int array)"; ok=0; }; \
	$(SPINEL) test/infer/int_keyed_hash.rb -c --no-line-map -o "$$tmp/k.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile int_keyed_hash)"; exit 1; }; \
	grep -Eq 'sp_IntIntHash \* *lv_h' "$$tmp/k.c" || { echo "infer-test: FAIL (a slot with no array evidence lost its int-keyed hash)"; ok=0; }; \
	$(SPINEL) test/infer/int_table_ivar_param.rb -c --no-line-map -o "$$tmp/t.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile int_table_ivar_param)"; exit 1; }; \
	grep -Eq 'static (inline )?((__attribute__\(\(always_inline\)\)|SP_ALWAYS_INLINE) )?sp_int sp_F_s_add\(sp_int [A-Za-z_]+, sp_int [A-Za-z_]+\)' "$$tmp/t.c" || { echo "infer-test: FAIL (an int table on an ivar poisoned the helper it feeds)"; grep -E 'sp_F_s_add\(' "$$tmp/t.c" | head -1; ok=0; }; \
	grep -Eq 'sp_PtrArray \* *iv_t;' "$$tmp/t.c" || { echo "infer-test: FAIL (the ivar table lost its typed representation)"; ok=0; }; \
	grep -Eq 'sp_IntArray \* *lv_row' "$$tmp/t.c" || { echo "infer-test: FAIL (a row read out of the table stayed boxed)"; ok=0; }; \
	$(SPINEL) test/infer/class_method_table_arg.rb -c --no-line-map -o "$$tmp/m.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile class_method_table_arg)"; exit 1; }; \
	grep -Eq 'sp_PtrArray \* *lv_rows' "$$tmp/m.c" || { echo "infer-test: FAIL (a table passed to a class method lost its typed representation)"; grep -E 'sp_M_s_consume\(' "$$tmp/m.c" | head -1; ok=0; }; \
	grep -Eq 'static (inline )?((__attribute__\(\(always_inline\)\)|SP_ALWAYS_INLINE) )?sp_int sp_F_s_mul\(sp_int [A-Za-z_]+, sp_int [A-Za-z_]+\)' "$$tmp/m.c" || { echo "infer-test: FAIL (a helper reading an element of the table bound a boxed parameter)"; grep -E 'sp_F_s_mul\(' "$$tmp/m.c" | head -1; ok=0; }; \
	$(SPINEL) test/infer/return_table_across_methods.rb -c --no-line-map -o "$$tmp/r.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile return_table_across_methods)"; exit 1; }; \
	grep -Eq 'static (inline )?((__attribute__\(\(always_inline\)\)|SP_ALWAYS_INLINE) )?sp_PtrArray \* *sp_T_s_build\(' "$$tmp/r.c" || { echo "infer-test: FAIL (a method returning a table of int arrays stayed a boxed poly array)"; grep -E 'sp_T_s_build\(' "$$tmp/r.c" | head -1; ok=0; }; \
	grep -Eq 'sp_PtrArray \* *lv_rows' "$$tmp/r.c" || { echo "infer-test: FAIL (the caller's table did not follow the callee's return type)"; ok=0; }; \
	grep -Eq 'static (inline )?((__attribute__\(\(always_inline\)\)|SP_ALWAYS_INLINE) )?sp_int sp_F_s_mul\(sp_int [A-Za-z_]+, sp_int [A-Za-z_]+\)' "$$tmp/r.c" || { echo "infer-test: FAIL (the narrowing was not visible while the helper's parameters bound)"; grep -E 'sp_F_s_mul\(' "$$tmp/r.c" | head -1; ok=0; }; \
	$(SPINEL) test/infer/ctor_table_arg.rb -c --no-line-map -o "$$tmp/ca.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile ctor_table_arg)"; exit 1; }; \
	grep -Eq 'sp_PtrArray \* *lv_rows' "$$tmp/ca.c" || { echo "infer-test: FAIL (a table handed to a constructor lost its typed representation)"; grep -oE 'sp_[A-Za-z]+Array \* *lv_rows' "$$tmp/ca.c" | head -1; ok=0; }; \
	grep -Eq 'sp_PtrArray \* *iv_t;' "$$tmp/ca.c" || { echo "infer-test: FAIL (the ivar the constructor stored the table in stayed boxed)"; ok=0; }; \
	grep -Eq 'sp_IntArray \* *lv_row' "$$tmp/ca.c" || { echo "infer-test: FAIL (a row read out of the constructor-assigned table stayed boxed)"; ok=0; }; \
	grep -Eq 'static (inline )?((__attribute__\(\(always_inline\)\)|SP_ALWAYS_INLINE) )?sp_int sp_F_s_mul\(sp_int [A-Za-z_]+, sp_int [A-Za-z_]+\)' "$$tmp/ca.c" || { echo "infer-test: FAIL (a helper reading an element of the constructor-assigned table bound a boxed parameter)"; grep -E 'sp_F_s_mul\(' "$$tmp/ca.c" | head -1; ok=0; }; \
	grep -Eq 'sp_PtrArray \* *lv_bare' "$$tmp/ca.c" || { echo "infer-test: FAIL (a table handed to a RECEIVERLESS new(...) lost its typed representation)"; grep -oE 'sp_[A-Za-z]+Array \* *lv_bare' "$$tmp/ca.c" | head -1; ok=0; }; \
	grep -Eq 'sp_PtrArray \* *iv_u;' "$$tmp/ca.c" || { echo "infer-test: FAIL (the ivar a receiverless new(...) stored the table in stayed boxed)"; ok=0; }; \
	grep -Eq 'sp_IntArray \* *lv_urow' "$$tmp/ca.c" || { echo "infer-test: FAIL (a row read out of the receiverless-constructed table stayed boxed)"; ok=0; }; \
	$(SPINEL) test/infer/map_table_rows.rb -c --no-line-map -o "$$tmp/mtr.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile map_table_rows)"; exit 1; }; \
	grep -Eq 'sp_PtrArray \* *lv_picked' "$$tmp/mtr.c" || { echo "infer-test: FAIL (a table of rows built by map lost its typed representation)"; grep -oE 'sp_[A-Za-z]+Array \* *lv_picked' "$$tmp/mtr.c" | head -1; ok=0; }; \
	grep -Eq 'sp_PtrArray \* *iv_rows;' "$$tmp/mtr.c" || { echo "infer-test: FAIL (the ivar holding a mapped table stayed boxed)"; ok=0; }; \
	grep -Eq 'sp_IntArray \* *lv_row ' "$$tmp/mtr.c" || { echo "infer-test: FAIL (a row read out of a mapped table stayed boxed)"; ok=0; }; \
	grep -Eq 'sp_PolyArray \* *lv_rows' "$$tmp/mtr.c" || { echo "infer-test: FAIL (a table mapped from a HASH must stay boxed -- its emitter cannot build a pointer array, and narrowing it stops the program running)"; grep -oE 'sp_[A-Za-z]+Array \* *lv_rows' "$$tmp/mtr.c" | head -1; ok=0; }; \
	grep -Eq 'static (inline )?((__attribute__\(\(always_inline\)\)|SP_ALWAYS_INLINE) )?sp_int sp_F_s_mul\(sp_int [A-Za-z_]+, sp_int [A-Za-z_]+\)' "$$tmp/mtr.c" || { echo "infer-test: FAIL (a helper reading an element of a mapped table bound a boxed parameter)"; grep -E 'sp_F_s_mul\(' "$$tmp/mtr.c" | head -1; ok=0; }; \
	$(SPINEL) test/nested_table_iter.rb -c --no-line-map -o "$$tmp/nti.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile nested_table_iter)"; exit 1; }; \
	grep -Eq 'sp_mul\(sp_PtrArray \* *lv_a, sp_PtrArray \* *lv_b\)' "$$tmp/nti.c" || { echo "infer-test: FAIL (a nested table passed as a method argument stayed boxed)"; grep -E 'sp_mul\(' "$$tmp/nti.c" | head -1; ok=0; }; \
	grep -Eq 'sp_FloatArray \* *lv_bj' "$$tmp/nti.c" || { echo "infer-test: FAIL (each_with_index on that argument did not yield a float row)"; ok=0; }; \
	grep -Eq 'sp_FloatArray_get\(lv_bj,' "$$tmp/nti.c" || { echo "infer-test: FAIL (the zip read the yielded row through the poly accessor)"; ok=0; }; \
	grep -Eq 'sp_PtrArray \* *lv_c' "$$tmp/nti.c" || { echo "infer-test: FAIL (each_with_index on the result table left it boxed)"; ok=0; }; \
	grep -Eq 'sp_FloatArray \* *lv_ci' "$$tmp/nti.c" || { echo "infer-test: FAIL (each_with_index on the result table did not yield a float row)"; ok=0; }; \
	grep -Eq 'sp_PtrArray \* *lv_other' "$$tmp/nti.c" || { echo "infer-test: FAIL (a nested table passed as zip'\''s other operand stayed boxed)"; ok=0; }; \
	grep -Eq 'sp_IntArray \* *lv_o' "$$tmp/nti.c" || { echo "infer-test: FAIL (zip did not yield an int row from that operand)"; ok=0; }; \
	grep -q 'sp_poly_arr_get' "$$tmp/nti.c" && grep -v -E 'lv_[abc]__bp[0-9]+ = sp_poly_arr_get\(' "$$tmp/nti.c" | grep -q 'sp_poly_arr_get' && { echo "infer-test: FAIL (a nested-table iterator still read through sp_poly_arr_get)"; ok=0; }; \
	grep -Eq 'sp_PtrArray \* *lv_rows' "$$tmp/nti.c" || { echo "infer-test: FAIL (the int table walked by each lost its typed representation)"; ok=0; }; \
	grep -Eq 'sp_PtrArray \* *lv_frows' "$$tmp/nti.c" || { echo "infer-test: FAIL (the float table walked by each lost its typed representation)"; ok=0; }; \
	grep -Eq 'lv_[abc]__bp[0-9]+ = sp_PtrArray_get' "$$tmp/nti.c" && { echo "infer-test: FAIL (a row pointer was assigned to a parameter of a split block)"; ok=0; }; \
	$(SPINEL) test/infer/nested_row_poly_param.rb -c --no-line-map -o "$$tmp/nrp.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile nested_row_poly_param)"; exit 1; }; \
	grep -Eq 'lv_r = sp_box_nullable_obj\(\(void \*\)\((sp_PtrArray_get\(lv_rows,|lv_r__bpin\))' "$$tmp/nrp.c" || { echo "infer-test: FAIL (a boxed block parameter over a nested table was not given the row pointer)"; ok=0; }; \
	grep -q 'lv_r = sp_PtrArray_get' "$$tmp/nrp.c" && { echo "infer-test: FAIL (a void * row was assigned straight into the boxed parameter)"; ok=0; }; \
	$(SPINEL) test/infer/generator_element_cycle.rb -c --no-line-map -o "$$tmp/g.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile generator_element_cycle)"; exit 1; }; \
	grep -Eq 'static (inline )?((__attribute__\(\(always_inline\)\)|SP_ALWAYS_INLINE) )?sp_int sp_F_s_add\(sp_int [A-Za-z_]+, sp_int [A-Za-z_]+\)' "$$tmp/g.c" || { echo "infer-test: FAIL (a generator whose element feeds back into its own operands latched a poly array)"; grep -E 'sp_F_s_add\(' "$$tmp/g.c" | head -1; ok=0; }; \
	grep -Eq 'static (inline )?((__attribute__\(\(always_inline\)\)|SP_ALWAYS_INLINE) )?sp_IntArray \* *sp_E_s_add\(sp_IntArray \*' "$$tmp/g.c" || { echo "infer-test: FAIL (the extension-field add did not settle on the Integer array)"; grep -E 'sp_E_s_add\(' "$$tmp/g.c" | head -1; ok=0; }; \
	$(SPINEL) test/infer/hash_new_method_value.rb -c --no-line-map -o "$$tmp/hn.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile hash_new_method_value)"; exit 1; }; \
	grep -Eq 'sp_StrStrHash \* *lv_s' "$$tmp/hn.c" || { echo "infer-test: FAIL (a returned Hash.new lost the variant its caller narrowed it to)"; grep -oE 'sp_[A-Za-z]+Hash \* *lv_s' "$$tmp/hn.c" | head -1; ok=0; }; \
	grep -Eq 'sp_StrIntHash \* *lv_i' "$$tmp/hn.c" || { echo "infer-test: FAIL (a returned Hash.new lost its narrowed value type)"; grep -oE 'sp_[A-Za-z]+Hash \* *lv_i' "$$tmp/hn.c" | head -1; ok=0; }; \
	grep -Eq 'sp_PolyPolyHash \* *sp_free_hash' "$$tmp/hn.c" || { echo "infer-test: FAIL (a returned Hash.new that nothing narrows lost the widest variant)"; ok=0; }; \
	$(SPINEL) test/infer/method_capture_dispatch_int.rb -c --no-line-map -o "$$tmp/mcd.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile method_capture_dispatch_int)"; exit 1; }; \
	grep -Eq 'sp_Bus_poke_ram\(sp_Bus \*self, sp_int lv_addr, sp_int lv_data\)' "$$tmp/mcd.c" || { echo "infer-test: FAIL (a captured method called only with Integers through a dispatch table lost its sp_int parameters)"; grep -E 'sp_Bus_poke_ram\(' "$$tmp/mcd.c" | head -1; ok=0; }; \
	$(SPINEL) test/infer/dead_constructor_no_arm.rb -c --no-line-map -o "$$tmp/dc.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile dead_constructor_no_arm)"; exit 1; }; \
	grep -q 'sp_poly_add' "$$tmp/dc.c" && { echo "infer-test: FAIL (a class constructed only in dead code widened a poly receiver's field read to poly)"; ok=0; }; \
	grep -Eq 'sp_int_add\(\(\{ sp_RbVal _t[0-9]+ = lv_d; sp_int _t[0-9]+ = (0|SP_INT_NIL); switch' "$$tmp/dc.c" || { echo "infer-test: FAIL (the field read of a boxed receiver did not stay an int switch, or its receiver is rooted for an arm that cannot run)"; ok=0; }; \
	rounds=$$(SP_FIXPOINT_LOG=1 $(SPINEL) test/infer/fixpoint_converges.rb -c --no-line-map -o "$$tmp/fp.c" 2>&1 | sed -n 's/^\[fp\] rounds=\([0-9]*\).*/\1/p' | tail -1); \
	case "$$rounds" in ''|*[!0-9]*) echo "infer-test: FAIL (no fixpoint round count -- SP_FIXPOINT_LOG gone?)"; ok=0;; \
	  *) [ "$$rounds" -lt 128 ] || { echo "infer-test: FAIL (the inference fixpoint ran to its $$rounds-round cap: it stopped mid-oscillation, and where it stops decides which typing is emitted)"; ok=0; };; \
	esac; \
	$(SPINEL) test/infer/inline_force_fanout.rb -c --no-line-map -o "$$tmp/iff.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile inline_force_fanout)"; exit 1; }; \
	grep -q 'SP_ALWAYS_INLINE [^(]* sp_f4(' "$$tmp/iff.c" && { echo "infer-test: FAIL (forced inlining copied a small-method chain past the size budget)"; ok=0; }; \
	$(SPINEL) test/infer/block_kept_through_or.rb -c --no-line-map -o "$$tmp/bko.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile block_kept_through_or)"; exit 1; }; \
	grep -q 'sp_RbVal lv_okaa' "$$tmp/bko.c" || { echo "infer-test: FAIL (a block kept through the left of a || did not widen its parameters)"; ok=0; }; \
	grep -q 'sp_int lv_alaa' "$$tmp/bko.c" || { echo "infer-test: FAIL (the left of an && widened a block parameter it does not let go)"; ok=0; }; \
	grep -q 'sp_int lv_opaa' "$$tmp/bko.c" || { echo "infer-test: FAIL (a || a predicate reads widened a block parameter it does not let go)"; ok=0; }; \
	grep -q 'sp_int lv_onaa' "$$tmp/bko.c" || { echo "infer-test: FAIL (a || nested in the left of an && widened a block parameter it does not let go)"; ok=0; }; \
	grep -q 'sp_int lv_oraa' "$$tmp/bko.c" || { echo "infer-test: FAIL (the right of a || a predicate reads widened a block parameter it does not let go)"; ok=0; }; \
	grep -q 'sp_RbVal lv_orkaa' "$$tmp/bko.c" || { echo "infer-test: FAIL (a block kept through the right of a parenthesized || did not widen its parameters)"; ok=0; }; \
	$(SPINEL) test/infer/yield_splat_int_params.rb -c --no-line-map -o "$$tmp/ysi.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile yield_splat_int_params)"; exit 1; }; \
	for m in 'sp_int lv_e = ' 'sp_int lv_f = ' 'sp_int_mul(lv_e, lv_f)'; do \
	  grep -q "$$m" "$$tmp/ysi.c" || { echo "infer-test: FAIL (yield(*xs) of an Integer array left a block parameter boxed: $$m)"; ok=0; }; \
	done; \
	grep -q 'sp_int lv_a = ' "$$tmp/ysi.c" && grep -q 'sp_int lv_b = ' "$$tmp/ysi.c" || { echo "infer-test: FAIL (a yield(*xs) reached before xs is typed boxed its block parameters for good)"; ok=0; }; \
	grep -q 'sp_box_int_or_nil(lv_h)' "$$tmp/ysi.c" || { echo "infer-test: FAIL (a block parameter yield(*xs) may leave without a value is not marked nullable)"; ok=0; }; \
	grep -q 'sp_box_int(lv_d)' "$$tmp/ysi.c" || { echo "infer-test: FAIL (the parameters of yield(*[i, i + 1]) test for a nil they cannot hold)"; ok=0; }; \
	$(SPINEL) test/infer/yield_nil_int_params.rb -c --no-line-map -o "$$tmp/yni.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile yield_nil_int_params)"; exit 1; }; \
	for m in 'sp_int lv_a = ' 'sp_int lv_d = ' 'sp_int lv_f = ' 'sp_int lv_g = '; do \
	  grep -q "$$m" "$$tmp/yni.c" || { echo "infer-test: FAIL (a block parameter bound an Integer and nil is boxed: $$m)"; ok=0; }; \
	done; \
	grep -q 'lv_a = SP_INT_NIL;' "$$tmp/yni.c" && grep -q 'lv_f = SP_INT_NIL;' "$$tmp/yni.c" || { echo "infer-test: FAIL (a nil yielded into an Integer block parameter is not its sentinel)"; ok=0; }; \
	grep -q 'lv_d = (1 < .*sp_poly_as_int_or_nil(' "$$tmp/yni.c" || { echo "infer-test: FAIL (a nil element of yield(*[i, nil]) is not unboxed to the sentinel)"; ok=0; }; \
	grep -q 'sp_int lv_g = (argc > 0) ? (_sp_proc_poly_args\[0\].tag == SP_TAG_NIL ? SP_INT_NIL : args\[0\])' "$$tmp/yni.c" || { echo "infer-test: FAIL (a proc prologue reads a nil argument as 0)"; ok=0; }; \
	$(SPINEL) test/infer/yield_nil_float_params.rb -c --no-line-map -o "$$tmp/ynf.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile yield_nil_float_params)"; exit 1; }; \
	for m in 'sp_float lv_a = ' 'sp_float lv_b = ' 'sp_float lv_c = ' 'sp_float lv_g = ' 'sp_float lv_k = ' 'sp_float lv_m = '; do \
	  grep -q "$$m" "$$tmp/ynf.c" || { echo "infer-test: FAIL (a Float block parameter bound nothing or nil is boxed: $$m)"; ok=0; }; \
	done; \
	grep -q 'sp_box_float_or_nil(lv_f)' "$$tmp/ynf.c" || { echo "infer-test: FAIL (a Float block parameter yield(*xs) may leave without a value is not marked nullable)"; ok=0; }; \
	grep -q 'lv_c = sp_float_nil();' "$$tmp/ynf.c" && grep -q 'lv_k = sp_float_nil();' "$$tmp/ynf.c" || { echo "infer-test: FAIL (a nil yielded into a Float block parameter is not its sentinel)"; ok=0; }; \
	grep -q 'lv_g = (1 < .*sp_poly_as_float_or_nil(' "$$tmp/ynf.c" || { echo "infer-test: FAIL (a nil element of yield(*[x, nil]) is not unboxed to the Float sentinel)"; ok=0; }; \
	grep -q 'sp_float lv_m = (argc > 0) ? sp_poly_to_f_or_nil(_sp_proc_poly_args\[0\])' "$$tmp/ynf.c" || { echo "infer-test: FAIL (a proc prologue reads a nil Float argument as 0.0)"; ok=0; }; \
	$(SPINEL) test/infer/yield_splat_rows_sure.rb -c --no-line-map -o "$$tmp/ysr.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (compile yield_splat_rows_sure)"; exit 1; }; \
	for m in 'sp_int lv_a = ' 'sp_int lv_b = ' 'sp_int lv_c = ' 'sp_int_mul(lv_b, lv_c)'; do \
	  grep -q "$$m" "$$tmp/ysr.c" || { echo "infer-test: FAIL (yield(*row) of an Integer table's row left a block parameter boxed: $$m)"; ok=0; }; \
	done; \
	grep -q 'sp_box_int_or_nil(lv_d)' "$$tmp/ysr.c" || { echo "infer-test: FAIL (a block parameter a short row may leave without a value is not marked nullable)"; ok=0; }; \
	grep -q 'lv_g = _t[0-9]*->data\[_t[0-9]*->start+0\];' "$$tmp/ysr.c" && grep -q 'lv_h = _t[0-9]*->data\[_t[0-9]*->start+1\];' "$$tmp/ysr.c" || { echo "infer-test: FAIL (yield(*pair) of a literal local no one changes tests a length it cannot lack)"; ok=0; }; \
	grep -q 'sp_int_mul(lv_g, lv_h)' "$$tmp/ysr.c" && grep -q 'sp_box_int(lv_g)' "$$tmp/ysr.c" || { echo "infer-test: FAIL (yield(*pair) of a literal local no one changes binds a nil it cannot hold)"; ok=0; }; \
	$(SPINEL) test/infer/array_nil_flag_plain_store.rb -c --no-line-map -o "$$tmp/anf.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (array_nil_flag_plain_store: -c)"; ok=0; }; \
	! grep -q '_nilable(' "$$tmp/anf.c" && grep -q '\] = _t[0-9]*; else sp_IntArray_set(lv_a, ' "$$tmp/anf.c" && grep -q 'sp_IntArray_push(lv_b, ' "$$tmp/anf.c" && grep -q 'sp_FloatArray_set(lv_f, ' "$$tmp/anf.c" || { echo "infer-test: FAIL (a loop storing numbers into a typed array took the nil-flag-setting store)"; ok=0; }; \
	grep -q 'sp_IntArray_push(lv_c, sp_IntArray_get(lv_a, ' "$$tmp/anf.c" && grep -q 'sp_IntArray_push(lv_q, sp_IntArray_pop(lv_b))' "$$tmp/anf.c" && grep -q 'sp_IntArray_push(_t[0-9]*, self->iv_v)\|sp_IntArray_push(lv_out, self->iv_v)' "$$tmp/anf.c" || { echo "infer-test: FAIL (copying an element or an ivar into a typed array took the nil-flag-setting store)"; ok=0; }; \
	grep -q 'sp_IntArray_sum(sp_IntArray_nil_sum_if_flagged(lv_a, 0), 0)' "$$tmp/anf.c" && grep -q 'sp_IntArray_max(sp_IntArray_nil_cmp_if_flagged(lv_b))' "$$tmp/anf.c" || { echo "infer-test: FAIL (a whole-array sum or max does not ask the nil flag)"; ok=0; }; \
	grep -q 'sp_IntArray_sum(sp_IntArray_nil_sum_ck(lv_m, 0), 0)' "$$tmp/anf.c" || { echo "infer-test: FAIL (the sum of an array analyze marked does not scan for nil)"; ok=0; }; \
	$(SPINEL) test/infer/nil_narrowing_reads.rb -c --no-line-map -o "$$tmp/nnr.c" >/dev/null 2>&1 || { echo "infer-test: FAIL (nil_narrowing_reads: -c)"; ok=0; }; \
	grep -q 'if ((lv_w > 2LL))' "$$tmp/nnr.c" && grep -q 'if ((lv_v > lv_k))' "$$tmp/nnr.c" || { echo "infer-test: FAIL (a read a guard or an in-bounds index proves non-nil still tests for nil)"; ok=0; }; \
	grep -q 'SP_INT_NIL_CMP_CK(_t[0-9]*, 0, ">"); _t[0-9]* > _t[0-9]*_r; })' "$$tmp/nnr.c" && grep -q 'SP_INT_NIL_CMP_CK(_t[0-9]*, 0, "<"); _t[0-9]* < _t[0-9]*_r; })' "$$tmp/nnr.c" || { echo "infer-test: FAIL (a narrowed read of a nilable local does not keep the other operand's half of the test)"; ok=0; }; \
	grep -q 'sp_int _t[0-9]* = lv_gv, _t[0-9]*_r = 0LL; SP_INT_NIL_CMP_CK(_t[0-9]*, _t[0-9]*_r, ">")' "$$tmp/nnr.c" || { echo "infer-test: FAIL (an in-bounds read of an array a write past the end can leave a nil in lost its test)"; ok=0; }; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "infer-test: pass"; else exit 1; fi

# SP_COLLECT_ERRORS recovers from an unsupported construct with a longjmp, and
# has to put back everything the abandoned unit was pointing at. The globals it
# missed pointed INTO that unit's stack frame, so the next unit's emission read
# a dead frame: a SIGSEGV whose site moved with the optimization level, and,
# short of that, a later method silently emitted with the wrong return
# convention. Both are checked here (#4141). tools/refusals.sh then compares
# every message test/reject/ and test/collect/ print, in both overflow modes,
# with test/collect/refusals.expected.
collect-errors-test: $(SPINEL)
	@tmp=$$(mktemp -d /tmp/spinel-collect.XXXXXX); ok=1; \
	src=test/collect/gap_inside_capturing_proc.rb; \
	SP_COLLECT_ERRORS=1 $(SPINEL) "$$src" -c --no-line-map -o "$$tmp/g.c" >"$$tmp/g.err" 2>&1; rc=$$?; \
	if [ $$rc -ne 0 ]; then echo "collect-errors-test: FAIL (rc=$$rc; a unit abandoned by the longjmp left a global pointing into its dead frame)"; sed -n 1,3p "$$tmp/g.err"; rm -rf "$$tmp"; exit 1; fi; \
	grep -q 'unsupported class variable read' "$$tmp/g.err" || { echo "collect-errors-test: FAIL (the gap was not reported at all, so nothing was recovered from)"; ok=0; }; \
	sed -n '/^static inline .* sp_b(const char \* lv_scheme) {/,/^}/p' "$$tmp/g.c" >"$$tmp/b.c"; \
	[ -s "$$tmp/b.c" ] || { echo "collect-errors-test: FAIL (the unit after the abandoned one was not emitted)"; ok=0; }; \
	grep -q 'return ' "$$tmp/b.c" || { echo "collect-errors-test: FAIL (the unit after the abandoned one lost its return)"; ok=0; }; \
	! grep -q '_sp_proc_poly_ret' "$$tmp/b.c" || { echo "collect-errors-test: FAIL (a plain method inherited the abandoned proc's return funnel)"; ok=0; }; \
	! grep -q '_cap)->c_x' "$$tmp/b.c" || { echo "collect-errors-test: FAIL (a plain method read its local through the abandoned proc's capture struct)"; ok=0; }; \
	seed=test/collect/seed; \
	SP_COLLECT_ERRORS=1 $(SPINEL) -c --rbs "$$seed" "$$seed/main.rb" -o "$$tmp/s.c" >"$$tmp/s.err" 2>&1; rc=$$?; \
	[ $$rc -ne 0 ] || { echo "collect-errors-test: FAIL (a contradicted --rbs seed was collected and then emitted anyway)"; ok=0; }; \
	n=$$(grep -c 'seed contradicted' "$$tmp/s.err"); \
	[ "$$n" -eq 2 ] || { echo "collect-errors-test: FAIL (collect mode reported $$n of 2 contradicted seeds)"; ok=0; }; \
	$(SPINEL) -c --rbs "$$seed" "$$seed/main.rb" -o "$$tmp/s2.c" >"$$tmp/s2.err" 2>&1; rc=$$?; \
	n=$$(grep -c 'seed contradicted' "$$tmp/s2.err"); \
	[ "$$n" -eq 2 ] || { echo "collect-errors-test: FAIL (without the flag the run reported $$n of 2 contradictions: every compile collects)"; ok=0; }; \
	[ $$rc -ne 0 ] || { echo "collect-errors-test: FAIL (a contradicted seed was emitted without the flag)"; ok=0; }; \
	$(SPINEL) "$$src" -c --no-line-map -o "$$tmp/g2.c" >"$$tmp/g2.err" 2>&1; rc=$$?; \
	[ $$rc -ne 0 ] || { echo "collect-errors-test: FAIL (a program with a gap compiled without the flag)"; ok=0; }; \
	[ ! -f "$$tmp/g2.c" ] || { echo "collect-errors-test: FAIL (a refused program's C was written)"; ok=0; }; \
	grep -q 'refusal, nothing written' "$$tmp/g2.err" || { echo "collect-errors-test: FAIL (the run did not close with the refusal count)"; ok=0; }; \
	rm -rf "$$tmp"; \
	tools/refusals.sh || ok=0; \
	if [ $$ok -eq 1 ]; then echo "collect-errors-test: pass"; else exit 1; fi

# The refusals the corpus prints (tools/refusals.sh --corpus): only the
# programs that refuse are listed. It compiles the whole corpus twice, so it
# is its own target rather than part of collect-errors-test.
refusals-corpus-test: $(SPINEL)
	@tools/refusals.sh --corpus

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
	$(SPINEL) test/alloc-report/signal_dump.rb -o "$$tmp/sig" >/dev/null 2>&1 || { echo "alloc-report-test: FAIL (compile signal_dump)"; exit 1; }; \
	SPINEL_ALLOC_REPORT="$$tmp/sig.folded" "$$tmp/sig" >/dev/null 2>&1 & \
	sigpid=$$!; sleep 1; kill -USR1 $$sigpid 2>/dev/null; sleep 1; \
	kill -0 $$sigpid 2>/dev/null || { echo "alloc-report-test: FAIL (the signal ended the program instead of dumping)"; ok=0; }; \
	first=$$(awk '/^alloc;.*String /{print $$NF; exit}' "$$tmp/sig.folded" 2>/dev/null); \
	[ -n "$$first" ] || { echo "alloc-report-test: FAIL (no report from a running program)"; ok=0; }; \
	sleep 1; kill -USR1 $$sigpid 2>/dev/null; sleep 1; \
	second=$$(awk '/^alloc;.*String /{print $$NF; exit}' "$$tmp/sig.folded" 2>/dev/null); \
	kill -9 $$sigpid 2>/dev/null; wait $$sigpid 2>/dev/null; \
	[ -n "$$second" ] && [ "$$second" -gt "$${first:-0}" ] || { echo "alloc-report-test: FAIL (the second signal did not re-dump a later table: $$first then $$second)"; ok=0; }; \
	$(SPINEL) test/alloc-report/signal_dump_idle.rb -o "$$tmp/idle" >/dev/null 2>&1 || { echo "alloc-report-test: FAIL (compile signal_dump_idle)"; exit 1; }; \
	SPINEL_ALLOC_REPORT="$$tmp/idle.folded" "$$tmp/idle" >/dev/null 2>&1 & \
	idlepid=$$!; sleep 1; kill -USR1 $$idlepid 2>/dev/null; sleep 1; \
	kill -0 $$idlepid 2>/dev/null || { echo "alloc-report-test: FAIL (the signal ended the idle program)"; ok=0; }; \
	grep -qE '^alloc;.*String [0-9]+$$' "$$tmp/idle.folded" 2>/dev/null || { echo "alloc-report-test: FAIL (an idle program did not report when signalled: the dump is waiting for an allocation that will never come)"; ok=0; }; \
	kill -9 $$idlepid 2>/dev/null; wait $$idlepid 2>/dev/null; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "alloc-report-test: pass"; else exit 1; fi

# spin end-to-end: scaffold/path-dep/git-dep/lock/vendor/offline/test,
# hermetic under a mktemp dir (tools/spin_e2e.sh).
# cident: every corpus program's C is byte-identical to <REF>'s (#7100's
# restructuring keeps this at 0 differing for every commit). The reference
# C is cached under build/cident/<sha>/.  Usage: make cident REF=HEAD~1
REF ?= HEAD~1
plan-check-test: $(SPINEL)
	@tools/plan_check.sh

repr-check-test: $(SPINEL)
	@tools/repr_check.sh

cident: $(SPINEL)
	@tools/cident.sh $(REF)

spin-check: bin/spin
	@tools/spin_e2e.sh bin/spin

# Full pre-push gate: test || bench || optcarrot in parallel.
gate:
	+@$(MAKE) --no-print-directory all
	+@$(MAKE) --no-print-directory gate-legs
	@echo "gate: ALL GREEN"

# The gate without the result cache: every program compiled and run.
gate-full:
	+@$(MAKE) --no-print-directory gate GATE_CACHE=0

gate-legs: gate-test gate-bench gate-optcarrot gate-rubyspec gate-props
gate-test:
	+@$(MAKE) --no-print-directory test OPT=-O1
# The property gates `check` runs. They were in the fast pre-commit target and
# NOT in the pre-push one, so the full gate was not a superset of the quick one
# and a representation regression could pass every leg of it. infer-test caught
# an Int-keyed hash losing its typed variant; nothing else did, for weeks.
# The property tests are independent, so one make runs them side by side
# under the gate's job server (they took 181 s one after another, the
# longest of the gate's legs; spin-check alone is 72 s).
gate-props:
	+@$(MAKE) --no-print-directory alloc-report-test infer-test collect-errors-test spin-check diff-test scale-test traits-check-test bop-arity-check-test

# The ty_traits table (types.c) against the functions each column names,
# for every builtin kind, in both integer-overflow modes.
# Each builtin-op row with a count range of its own against CRuby's accepted
# counts for its class and name (the arity table, codegen_call.c).
bop-arity-check-test: $(SPINEL)
	@$(SPINEL) --check-bop-arity

traits-check-test: $(SPINEL)
	@$(SPINEL) --check-traits -c test/box_random_argf.rb -o /dev/null && \
	 $(SPINEL) --check-traits --int-overflow=promote -c test/box_random_argf.rb -o /dev/null

# The front end's scaling, measured as work rather than time: the counting
# compiler analyzes one generated program at K units and at 4K, and the ratio
# of the two work counts is compared with a limit. A linear front end gives 4;
# a pass that rescans the node table per node, the regression that came back
# four times before anyone profiled it (rubys/roundhouse#72), pushes it well
# past. The count is deterministic, so the test does not depend on the
# machine or its load. The pair is K=100 and K=400: at 25 -> 100 most of
# what is still superlinear is too small to show, and real applications are
# larger than either (rubys in #5035; lobsters is ~78K lines as emitted).
# The limit sits just above today's ratio (4.68, down from 5.61 before the
# #5035 fixes). Lower it as the remaining superlinear passes are fixed.
SCALE_LIMIT ?= 5.2
# Not a timed test: the #5718 per-hop rescan gave ~450x here, and a work count cannot drift with the machine.
IE_FORWARD_LIMIT ?= 2.5
# The code generator's scaling: the same two programs compiled to C (-c), where
# the leg above stops after the analysis (--emit-rbs), so the count also holds
# every pass the emission runs. gen.sh's C grows linearly (4.03x at 4x the
# program) but the emission's work does not: the emission alone reads 13.9x
# and the whole compile 6.39x. Sampled every 4,096 units, about 470M of the
# 1.16G units above 4x are walks of the program repeated per call site or per
# method: singleton_def_of under arity_violation, whose table is cached but
# rebuilt, a walk of the node table, whenever the emission has added a node
# since the last call (401 rebuilds for 1,200 calls at 400 units),
# fi_fiber_stack_risk under emit_method_signature, and scope_has_return's scan
# of a node kind under call_breaks. The limit sits just above; lower it as
# those go. One walk of
# the node table per emitted method reads 7.66x. The count does not see a
# scan that compares ids before names: scope_is_shadowed's scan of every
# later scope, per class at every poly dispatch, was 11% of gen.sh 400's
# instructions, and answering it from a table (285b5a8ba) moved this leg
# from 6.40x to 6.39x.
SCALE_CODEGEN_LIMIT ?= 6.9
# The argument binder's and the block-parameter typing's scaling: the call
# shapes they plan site by site (test/scale/call_shapes.sh), compiled to C at
# N=25 and N=100 units. A buffer method a whole name group defines behind a
# POLY seed, Strings lent to a parameter that appends, poly dispatch with
# arguments that run, splat and keyword plans, yield(*row) into block
# parameters, nullable locals, escaping blocks and writes past an array's
# end: gen.sh has none of them, so a pass that walks the program per call
# site of one costs it nothing. #6135's first an_local_has_alias, a walk of
# the node table per call site of a lent parameter, leaves gen.sh at 6.39x
# and reads 5.40x here (5.20x against 4.32x on its own base); a walk per
# argument the binder runs first reads 4.98x. Today's 4.28x is what is left
# once a diverging yield's typing stopped walking the table per yield (4.55x
# before); the limit sits about 5% above, and the same two walks, put back on
# top of that, read 5.19x and 4.73x. The count sees node accesses and name
# compares, not a pass's scan of its own side table: #6183's handle registry,
# scanned per lookup, took 4,798 steps at N=100 against a billion, which no
# count or clock sees at a test's size.
CALL_SHAPES_LIMIT ?= 4.5
# Each count is taken only from a compile that succeeded (sw): spinel-work
# prints its count from an atexit handler, also when the compile fails, so
# reading it alone would accept the ratio of a program that did not build.
scale-test: $(SPINEL_WORK)
	@tmp=$$(mktemp -d /tmp/spinel-scale.XXXXXX); \
	sw () { o=$$($(SPINEL_WORK) "$$@" 2>&1); r=$$?; \
	  if [ $$r -ne 0 ]; then echo "scale-test: FAIL (spinel-work exited $$r: $$*)" >&2; echo "$$o" | tail -3 >&2; return 1; fi; \
	  echo "$$o" | sed -n 's/^spinel-work: //p'; }; \
	sh test/scale/gen.sh 100 > "$$tmp/a.rb"; sh test/scale/gen.sh 400 > "$$tmp/b.rb"; \
	wa=$$(sw --emit-rbs -o "$$tmp/a.rbs" "$$tmp/a.rb") || { rm -rf "$$tmp"; exit 1; }; \
	wb=$$(sw --emit-rbs -o "$$tmp/b.rbs" "$$tmp/b.rb") || { rm -rf "$$tmp"; exit 1; }; \
	ca=$$(sw -c -o "$$tmp/a.c" "$$tmp/a.rb") || { rm -rf "$$tmp"; exit 1; }; \
	cb=$$(sw -c -o "$$tmp/b.c" "$$tmp/b.rb") || { rm -rf "$$tmp"; exit 1; }; \
	sh test/scale/call_shapes.sh 25 > "$$tmp/s1.rb"; sh test/scale/call_shapes.sh 100 > "$$tmp/s4.rb"; \
	sa=$$(sw -c -o "$$tmp/s1.c" "$$tmp/s1.rb") || { rm -rf "$$tmp"; exit 1; }; \
	sb=$$(sw -c -o "$$tmp/s4.c" "$$tmp/s4.rb") || { rm -rf "$$tmp"; exit 1; }; \
	( ulimit -t 20; $(SPINEL_WORK) -c -o "$$tmp/hls.c" test/scale/hash_literal_sources_fanout.rb ) >/dev/null 2>&1 || \
	  { rm -rf "$$tmp"; echo "scale-test: FAIL (the hash-literal source walk revisited call sites along every path)"; exit 1; }; \
	sh test/scale/ie_forward_chain.sh 2 > "$$tmp/f2.rb"; sh test/scale/ie_forward_chain.sh 4 > "$$tmp/f4.rb"; \
	fa=$$(sw -c -o "$$tmp/f2.c" "$$tmp/f2.rb") || { rm -rf "$$tmp"; exit 1; }; \
	fb=$$(sw -c -o "$$tmp/f4.c" "$$tmp/f4.rb") || { rm -rf "$$tmp"; exit 1; }; \
	rm -rf "$$tmp"; \
	if [ -z "$$wa" ] || [ -z "$$wb" ] || [ -z "$$fa" ] || [ -z "$$fb" ] || \
	   [ -z "$$ca" ] || [ -z "$$cb" ] || [ -z "$$sa" ] || [ -z "$$sb" ]; then echo "scale-test: FAIL (the counting compiler reported no work count)"; exit 1; fi; \
	awk -v a="$$fa" -v b="$$fb" -v lim="$(IE_FORWARD_LIMIT)" 'BEGIN { r = b / a; \
	  printf "scale-test: instance_eval forwarding work at 2x the wrappers is %.2fx (limit %.2f)\n", r, lim; exit (r > lim) }' || \
	  { echo "scale-test: FAIL (the instance_eval forwarding walk grew superlinearly in the wrapper classes, see build_ie_map)"; exit 1; }; \
	awk -v a="$$wa" -v b="$$wb" -v lim="$(SCALE_LIMIT)" 'BEGIN { r = b / a; \
	  printf "scale-test: work at 4x the program is %.2fx (linear 4.00, limit %.2f)\n", r, lim; exit (r > lim) }' || \
	  { echo "scale-test: FAIL (the front end grew superlinearly: some pass rescans per node; profile per pass, see rubys/roundhouse#72)"; exit 1; }; \
	awk -v a="$$ca" -v b="$$cb" -v lim="$(SCALE_CODEGEN_LIMIT)" 'BEGIN { r = b / a; \
	  printf "scale-test: work at 4x the program, compiled to C, is %.2fx (limit %.2f)\n", r, lim; exit (r > lim) }' || \
	  { echo "scale-test: FAIL (the C emission grew superlinearly: some pass rescans per method or call site; compare the -c and --emit-rbs counts)"; exit 1; }; \
	awk -v a="$$sa" -v b="$$sb" -v lim="$(CALL_SHAPES_LIMIT)" 'BEGIN { r = b / a; \
	  printf "scale-test: call-shape work at 4x the units, compiled to C, is %.2fx (linear 4.00, limit %.2f)\n", r, lim; exit (r > lim) }' || \
	  { echo "scale-test: FAIL (a binding or block-typing pass grew superlinearly: it rescans per call site or argument, see test/scale/call_shapes.sh)"; exit 1; }

# `spinel diff`, end to end, on the three answers the tool has to give: a
# program both runtimes agree on (exit 0), a documented divergence (exit 1,
# exception-diff) and a refusal (exit 2, compile-error). The normalization
# and the classifier have their own corpus tests (test/tools_diff_*.rb);
# this leg is the plumbing: the dispatch from `spinel diff`, the two runs,
# the report, and the scratch cleanup.
diff-test: $(SPINEL) bin/spinel-diff
	@ok=1; \
	if ! command -v ruby >/dev/null 2>&1; then echo "diff-test: skipped (needs ruby)"; exit 0; fi; \
	out=$$($(SPINEL) diff test/fixtures/diff/same.rb); rc=$$?; \
	[ $$rc -eq 0 ] && echo "$$out" | grep -q '^spinel diff: same$$' || { echo "diff-test: FAIL (same.rb: rc=$$rc)"; echo "$$out"; ok=0; }; \
	out=$$($(SPINEL) diff test/fixtures/diff/frozen_literal.rb); rc=$$?; \
	[ $$rc -eq 1 ] && echo "$$out" | grep -q '^spinel diff: exception-diff$$' && echo "$$out" | grep -q 'FrozenError' || { echo "diff-test: FAIL (frozen_literal.rb: rc=$$rc)"; echo "$$out"; ok=0; }; \
	out=$$($(SPINEL) diff test/fixtures/diff/refused.rb); rc=$$?; \
	[ $$rc -eq 2 ] && echo "$$out" | grep -q '^spinel diff: compile-error$$' || { echo "diff-test: FAIL (refused.rb: rc=$$rc)"; echo "$$out"; ok=0; }; \
	$(SPINEL) diff --emit-issue "$${TMPDIR:-/tmp}/spinel-diff-test.md" test/fixtures/diff/frozen_literal.rb >/dev/null; \
	grep -q '^spinel diff: exception-diff$$' "$${TMPDIR:-/tmp}/spinel-diff-test.md" || { echo "diff-test: FAIL (--emit-issue wrote no report)"; ok=0; }; \
	rm -f "$${TMPDIR:-/tmp}/spinel-diff-test.md"; \
	ls "$${TMPDIR:-/tmp}"/spinel-diff-*.rb.* >/dev/null 2>&1 && { echo "diff-test: FAIL (scratch files left behind)"; ls "$${TMPDIR:-/tmp}"/spinel-diff-*; ok=0; }; \
	$(SPINEL) diff /nonexistent.rb >/dev/null 2>&1; [ $$? -eq 4 ] || { echo "diff-test: FAIL (a missing file is the tool's own error, exit 4)"; ok=0; }; \
	[ $$ok -eq 1 ] && echo "diff-test: pass" || exit 1
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
	@# Every lib/*.h is installed, derived from the tree rather than
	@# enumerated: spinel_rt.h includes what it includes, and a hand-kept
	@# list goes stale the day a new header lands -- sp_process_status.h
	@# missed it and every installed toolchain failed to compile anything
	@# (#4186).
	for h in lib/*.h; do install -m 644 $$h $(SPNLDIR)/lib/; done
	install -d $(SPNLDIR)/lib/spinel
	install -m 644 lib/spinel/runtime.h  $(SPNLDIR)/lib/spinel/
	@# The wasm32-wasi shim headers go with the runtime headers; the target's
	@# archive goes too when `make wasm-rt` built it (packages' _wasi.o ride
	@# the packages copy below).
	install -d $(SPNLDIR)/lib/wasi/sys
	for h in lib/wasi/*.h; do install -m 644 $$h $(SPNLDIR)/lib/wasi/; done
	for h in lib/wasi/sys/*.h; do install -m 644 $$h $(SPNLDIR)/lib/wasi/sys/; done
	@if [ -f $(SP_RT_WASI_LIB) ]; then \
	  install -d $(SPNLDIR)/lib/wasm32-wasi; \
	  install -m 644 $(SP_RT_WASI_LIB) $(SPNLDIR)/lib/wasm32-wasi/; \
	fi
	rm -rf $(SPNLDIR)/packages
	cp -r packages $(SPNLDIR)/packages
	@# the core methods written in Ruby, spliced by the compiler beside packages/
	rm -rf $(SPNLDIR)/builtins
	cp -r builtins $(SPNLDIR)/builtins
	rm -rf $(SPNLDIR)/packages/*/build
	@# cp -r keeps each file's mode, and a package object built through
	@# sccache is 0640 whatever the umask (sccache 0.17 writes its outputs
	@# that way, on a cache hit too). Installed by root, every program
	@# requiring json, openssl, stringio, ... then failed to link for any
	@# other user (errno 13 on the .o). Everything here is read by whoever
	@# compiles, as the install -m 644 above already says for the runtime.
	chmod -R a+rX $(SPNLDIR)/packages $(SPNLDIR)/builtins
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
