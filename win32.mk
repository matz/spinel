# win32.mk -- the native Windows build: MinGW-w64 gcc against the UCRT, as
# MSYS2 and RubyInstaller's Devkit ship it. common.mk includes this file when
# $(OS) is Windows_NT, or when CC is a *-w64-mingw32-* cross compiler
# (`make CC=x86_64-w64-mingw32-gcc bin/spinel lib/libspinel_rt.a` on Linux);
# on every other host the PLATFORM_ variables
# it sets are empty and none of it is read.
#
# The runtime is written against POSIX. lib/win32 holds the POSIX surface
# MinGW leaves out or answers differently (see lib/win32/sp_win32.h): every
# compile sees it ahead of MinGW's own headers, and its objects go into the
# runtime archives and the compiler. Programs link statically, so one needs
# no MinGW DLL beside it.

# the rules below are read before the Makefile's own: `make` alone is still
# `make all`
.DEFAULT_GOAL := all

# there is no `cc`
ifeq ($(origin CC),default)
  CC = gcc
endif

# the shim's headers first; 64-bit st_size and offsets; no winsock.h through
# <windows.h> (the shim declares its own sockets)
# Windows 10: the stack-limit and other calls the shim makes are declared from 0x0602 on, and a mingw-w64 default is Windows 7 (0x0601)
PLATFORM_FLAGS = -Ilib/win32 -D_FILE_OFFSET_BITS=64 -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
# a cross toolchain (Debian's and Ubuntu's mingw-w64) defaults to the old
# msvcrt.dll; the native ones above target the UCRT, and so does the build:
# its headers say the UCRT's, and its import library goes ahead of msvcrt's
ifneq ($(OS),Windows_NT)
  PLATFORM_FLAGS += -D_UCRT -D__MSVCRT_VERSION__=0xE00
  PLATFORM_CRT    = -lucrt
endif
PLATFORM_HDRS  = $(wildcard lib/win32/*.h lib/win32/*/*.h)
# the winsock and CNG import libraries the shim calls
PLATFORM_LIBS  = -lws2_32 -lbcrypt -lwinpthread

PLATFORM_SRC    = lib/win32/sp_win32.c lib/win32/sp_win32_net.c lib/win32/sp_win32_ctx.c lib/win32/sp_win32_crypt.c lib/win32/sp_win32_driver.c
PLATFORM_OBJ    = $(patsubst lib/win32/%.c,build/win32/%.o,$(PLATFORM_SRC))
PLATFORM_MT_OBJ = $(patsubst lib/win32/%.c,build/mt/win32/%.o,$(PLATFORM_SRC))

build/win32/%.o: lib/win32/%.c $(PLATFORM_HDRS)
	@mkdir -p $(@D)
	$(CC) -c $(COPT) $(SEC_FLAGS) $< -o $@
build/mt/win32/%.o: lib/win32/%.c $(PLATFORM_HDRS)
	@mkdir -p $(@D)
	$(CC) -c $(COPT) $(SEC_FLAGS) $(MT_DEF) $< -o $@

# Static, with the shim's import libraries, and an 8 MB main-thread stack as
# Linux gives one (Windows' default is 1 MB); the compiler itself recurses
# deeper and gets 64 MB. -Llib/win32 is for the stub archives below.
LDFLAGS += -static $(PLATFORM_LIBS) $(PLATFORM_CRT) -Llib/win32 -Wl,--stack,8388608
SPINEL_LDFLAGS = -Wl,--stack,67108864

# libc's own link names (-lc for an ffi_lib "c", -lcrypt for String#crypt):
# the C runtime and the shim are what answers them here, and every link has
# both, so each is an empty archive. The driver passes -L<lib>/win32 too.
PLATFORM_STUB_LIBS = $(addprefix lib/win32/lib,$(addsuffix .a,c dl rt util crypt))
lib/win32/lib%.a:
	@rm -f $@
	ar rcs $@
lib/libspinel_rt.a lib/libspinel_rt_mt.a: | $(PLATFORM_STUB_LIBS)

# every bundled package object includes the runtime headers, and so the shim's
$(foreach p,$(wildcard packages/*/sp_*.c),$(p:.c=.o) $(p:.c=_mt.o)): $(PLATFORM_HDRS)

# spinel-timeout over CreateProcess, in place of the fork/SIGALRM one
SPINEL_TIMEOUT_SRC = lib/win32/spinel-timeout.c
SPINEL_TIMEOUT_LDFLAGS = -municode

# The corpus's shell command lines are written for a POSIX sh, and the shim
# runs `/bin/sh -c CMD` under cmd.exe as CRuby on Windows does; the harness
# runs under MSYS2, so its programs get that sh instead (SPINEL_SHELL).
ifeq ($(OS),Windows_NT)
export SPINEL_SHELL := $(shell cygpath -m /usr/bin/sh 2>/dev/null)
endif

# Tests of what only a POSIX system answers, which are not run here. The
# list is the one place the corpus knows about Windows: a test it names is
# skipped, so a test renamed or deleted only stops being skipped.
# permission bits: a file's mode is read-only-or-not on Windows
TEST_SKIP += packages/fileutils/test/fileutils_copy_file.rb packages/fileutils/test/fileutils_test.rb
TEST_SKIP += packages/tempfile/test/tempfile_test.rb
TEST_SKIP += packages/tmpdir/test/tmpdir_expand_usable.rb packages/tmpdir/test/tmpdir_parent_check.rb
# POSIX paths: /proc, /dev/fd, /etc, a root without a drive, "<" in a name
TEST_SKIP += packages/tmpdir/test/tmpdir_expand.rb
TEST_SKIP += test/file_expand_path.rb test/file_surface_extended.rb test/implicit_conversion_protocol.rb
TEST_SKIP += test/ffi_header_declared_extern.rb test/handle_nil_equality.rb
TEST_SKIP += test/process_spawn_failure_names_path.rb test/process_spawn_file_redirect.rb
TEST_SKIP += test/process_spawn_splat_and_pair.rb test/std_stream_stat.rb
# the user database knows the current user alone, as uid 0, and no group
TEST_SKIP += packages/etc/test/etc_each.rb packages/etc/test/etc_getgr.rb packages/etc/test/etc_getpw.rb
# no fork: Process.daemon is NotImplementedError there, as in CRuby
TEST_SKIP += test/process_daemon.rb
# no job control: SIGSTOP ends a child, so no wait sees one stopped
TEST_SKIP += test/process_wait_wuntraced.rb test/process_wait_flags_thread.rb
# FIFOs (File.mkfifo)
TEST_SKIP += test/fifo_open_does_not_pin_worker.rb test/issue_3118.rb
# Linux's numeric O_* flags; ELF-only asm; NTFS's 100 ns file times
TEST_SKIP += test/io_sysopen_flags.rb test/ffi_bool_return_low_byte.rb test/file_utime_nanoseconds.rb
# what RbConfig and Gem.win_platform? answer on Windows, as in CRuby there
TEST_SKIP += test/gem_rbconfig_stub.rb test/gem_toplevel_path.rb
# winsock accepts a whole non-blocking send while its buffer has room
TEST_SKIP += test/sp_net_write_partial.rb
# tools/cident.sh, a POSIX shell tool: its forks run past the 10 s limit under MSYS2
TEST_SKIP += test/tools_cident_abandoned.rb test/tools_cident_cache.rb test/tools_cident_concurrent.rb

# `make install`: the shim's headers and stub archives, which an installed
# compiler puts on every program's command line, and the overlays it reads
# after builtins/ and packages/ files (sp_w32_overlay)
PLATFORM_INSTALL = for d in . sys netinet arpa; do install -d $(SPNLDIR)/lib/win32/$$d; \
	  for h in lib/win32/$$d/*.h; do install -m 644 $$h $(SPNLDIR)/lib/win32/$$d/; done; done; \
	for a in $(PLATFORM_STUB_LIBS); do install -m 644 $$a $(SPNLDIR)/lib/win32/; done; \
	(cd lib/win32 && find overlay -name '*.rb') | while read f; do \
	  install -d $(SPNLDIR)/lib/win32/$$(dirname $$f); install -m 644 lib/win32/$$f $(SPNLDIR)/lib/win32/$$f; done
