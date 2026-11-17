# Native Windows

**Community-maintained, best effort.** The native Windows build is kept
working by its users, not by Spinel's maintainers: it may lag behind
`master`, and nothing in CI waits on it (the one check builds the compiler
and the runtime with MSYS2's UCRT64 gcc on a Windows runner and runs a small
program, and does not block a push). [WSL](https://learn.microsoft.com/windows/wsl/)
is the dependable way to run Spinel on Windows: there it builds and runs as
the Linux toolchain. Everything Windows needs lives in `lib/win32/` and
`win32.mk`, so that if it ever stands in the way of a POSIX change and is
not fixed promptly, a maintainer can remove it cheaply.

```sh
# in RubyInstaller's Devkit shell (`ridk enable`), or an MSYS2 UCRT64 shell
pacman -S --needed make diffutils curl tar git mingw-w64-ucrt-x86_64-gcc
# MSYS2 only (the Devkit has RubyInstaller's): the build runs Ruby generators
pacman -S --needed mingw-w64-ucrt-x86_64-ruby
make deps && make
bin/spinel app.rb          # -> app.exe
./app.exe
```

Spinel builds and runs on Windows as a native program: the compiler is
`spinel.exe`, the C it emits is compiled by MinGW-w64's gcc against the UCRT
-- the toolchain RubyInstaller's Devkit ships -- and the programs it builds
are ordinary Windows executables that need no DLL beyond the system's own
(the MinGW runtime is linked in statically). `spinel.exe` runs from
PowerShell or cmd.exe as well as from MSYS2: with no `gcc` on `PATH` it
uses the Devkit beside the `ruby` on `PATH` (RubyInstaller puts it there
only under `ridk enable`), then the installs RubyInstaller records in the
registry, then `C:\msys64`.

`spin` runs its build steps as POSIX shell command lines, so on Windows it
needs a POSIX `sh`: set `SPINEL_SHELL` to one, MSYS2's for instance
(`export SPINEL_SHELL=$(cygpath -m /usr/bin/sh)` in a Devkit or UCRT64
shell). Without it a shell command runs under cmd.exe, as CRuby's do.

## How the port is built

The runtime is written against POSIX, and it stays that way: everything
Windows needs is in `lib/win32/` and `win32.mk`, the arrangement `lib/wasi/`
is for wasm32-wasi.

- `lib/win32/` holds a header for each POSIX header MinGW leaves out or
  answers differently (`unistd.h`, `sys/mman.h`, `poll.h`, `spawn.h`,
  `ucontext.h`, `sys/socket.h`, `signal.h`, `dirent.h`, `sys/stat.h`,
  `time.h`, `stdio.h`, ...), each `#include_next`-ing MinGW's own where it
  has one, and the code behind them over Win32: `sp_win32.c` (files,
  processes, signals, `mmap`, ...), `sp_win32_net.c` (sockets as file
  descriptors, `poll`), `sp_win32_ctx.c` (the Windows x64 context switch),
  `sp_win32_crypt.c` (DES `crypt(3)`) and `sp_win32_driver.c` (the compiler
  driver's startup). The shim headers include neither `<windows.h>` nor
  `<winsock2.h>`; only those `.c` files do, so neither the runtime nor a
  generated program sees their macros.
- `win32.mk`, which `common.mk` includes when `$(OS)` is `Windows_NT`, sets
  the toolchain and the `PLATFORM_` variables the Makefile's rules take
  (`-Ilib/win32`, the shim's objects and libraries, the static link, the
  test marker below) -- empty on every other host -- and empty
  `lib/win32/lib{c,dl,rt,util,crypt}.a`, so a program's `-lc` or `-lcrypt`
  finds the C runtime and the shim that answer them here.
- The driver includes `lib/win32/sp_win32_driver.h` on Windows, which sets
  what it otherwise defaults to POSIX's: the compiler (`gcc`), the `.exe`,
  the flags every program gets, and a startup hook.

What `lib/` itself carries for Windows is small and named:

- **The coroutine switch** (`lib/sp_fiber_ctx.h`): Windows takes the
  `ucontext` fallback, which `lib/win32/ucontext.h` provides -- its x64 ABI
  saves more registers, and a switch there also moves the stack bounds the
  thread's TEB holds.
- **Starting a process** (`sp_process.c`, `sp_system.c`, `sp_cold.c`):
  there is no `fork`, so `Process.spawn`, `Kernel#system` and backticks have
  a `posix_spawn` path, which the shim implements over `CreateProcessW`;
  a POSIX build keeps its `fork` path and never compiles this one.
- **Path roots** (`sp_file_path_root_len` in `sp_alloc.h`): `File.expand_path`,
  `Dir.glob`, `File.absolute_path?`, `File.dirname` and the tmpdir package
  take `C:/` as a root as well as `/`; `Pathname.root_str` does the same.
- **One-liners**: the CSPRNG reads `getentropy` (as on WASI), which the shim
  answers from `BCryptGenRandom`; `File.birthtime` reads the creation time;
  `%a` goes through MinGW-w64's C99 printf; `RUBY_PLATFORM` is
  `x64-mingw-ucrt`, CRuby's spelling, and RbConfig and `Gem.win_platform?`
  answer for it.

## What is different on this target

- **Paths** are the ones CRuby on Windows answers: `Dir.pwd` and
  `File.expand_path` give `C:/Users/me/...` with forward slashes, and a
  backslash is accepted as a separator. Names are UTF-8 throughout (the shim
  calls the wide Win32 API). `/dev/null` means `NUL`, and `/tmp` is the
  system temporary directory (`%TEMP%`), as under MSYS2 and Cygwin.
- **Line endings**: files are read and written as bytes, as on POSIX --
  no CRLF translation, `"w"` and `"wb"` alike. CRuby on Windows opens a
  file in text mode unless asked for binary, so a file it writes with
  `puts` has CRLF line ends (and is that many bytes longer).
- **Shell commands** -- `system("...")`, backticks, a one-string
  `Process.spawn` -- run under `%ComSpec%` (cmd.exe), as CRuby's do on
  Windows. Setting `SPINEL_SHELL` to a POSIX `sh` runs them under that
  instead; the test harness does, under MSYS2. A program named by its POSIX
  path (`/bin/echo`, `/usr/bin/env`) is looked up on `PATH` by its name.
- **Signals** are delivered as POSIX delivers them: a process-directed one
  (the console's Ctrl-C, Ctrl-Break or close, which are `INT`, `BREAK` and
  `HUP`; a `Process.kill` from another thread) interrupts the main thread
  and runs its handler there -- through a special user APC on Windows 11
  and Server 2022, and by suspending the thread and redirecting it where
  those are missing -- and a `sleep` it interrupts returns early with
  `EINTR` underneath, as on POSIX. A signal is blocked while its own
  handler runs. `Process.kill` on a child ends it (`TerminateProcess`), and
  `$?` reads the signal it was sent; `INT`/`BREAK` reach a child started in
  its own process group as a console Ctrl-Break. A fault in the program --
  an overflowed stack included -- is delivered to its handler as SIGSEGV,
  on the alternate signal stack, so a deep recursion is a
  `SystemStackError` here too.
- **No `fork`**: `Process.fork` and a prefork server answer `Errno::ENOSYS`.
  `exec` runs the program and exits with its status when it ends (Windows
  cannot replace a process image), as the MSVC runtime's `_exec` does.
- **No `pty`**: CRuby builds no `ext/pty` on Windows, and there is no
  pseudo-terminal a POSIX pty package can open. The bundled `pty` package
  is not built (`PLATFORM_NO_PKG_OBJS` in `win32.mk`), and `require "pty"`
  finds no library, as for any library Spinel does not have: the compiler
  warns, and `PTY` is not defined.
- **Permissions and owners**: a file's mode is what Windows can say --
  read-only or not, and executable for `.exe`/`.com`/`.bat`/`.cmd` -- so
  `File.chmod(0600, f)` followed by `File.stat(f).mode` answers `0100644`.
  `chown` succeeds and changes nothing; `uid`/`gid` are 0; `Etc.getpwnam`
  knows the current user only.
- **Sockets** are file descriptors as on POSIX (`fdopen`, `IO.select`,
  non-blocking mode); `UNIXSocket` needs Windows 10 1803 or later.
  `SO_REUSEADDR` is accepted and has POSIX's meaning (winsock's own would
  let a second server take a port in use, so it is not passed through).
- **Symlinks** need Developer Mode or an elevated process, as for any
  Windows program.

## A library as a DLL

A library (`--ext-init`/`--ext-entry`, [spin.md](spin.md)) builds into a
DLL that a host loads with `LoadLibrary` -- Python's ctypes, a C#
`DllImport` -- over the same archives a shared object takes on Linux:
`make runtime-pic` builds them here too, and the library is compiled with
the flags `win32.mk` gives every compile.

```sh
gcc -shared -Ilib -Ilib/win32 -D_FILE_OFFSET_BITS=64 -DWIN32_LEAN_AND_MEAN \
  -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 kernel.c lib/libspinel_rt_pic.a \
  -static -lws2_32 -lbcrypt -lwinpthread -o kernel.dll
```

- **No position-independent build is needed**: x64 Windows code is
  relocated at load time, gcc ignores `-fPIC` there, and the `_pic`
  archives and package objects compile as the regular ones do. A threaded
  library links `lib/libspinel_rt_mt_pic.a` with `-DSP_THREADS`.
- **Thread-local state** is the native kind, which a DLL loaded at run time
  gets for every thread. Windows allocates it, about 600 KB a thread, for
  each thread of the host -- those the host started before loading the DLL
  included.
- **The DLL needs no DLL** beyond the system's own, as a program does
  (`-static`).
- **Exports**: with no `.def` file and no `__declspec(dllexport)`, the
  linker exports every global symbol of the DLL, the runtime's thousands
  included. A `.def` file that lists the init, its `_try` helper and the
  entries, given on the same command line, exports those alone:

  ```
  EXPORTS
  Init_kernel
  Init_kernel_try
  sp_Kernel_s_triple
  ```

## Tests

`make test-corpus` runs the corpus as on Linux, from an MSYS2 shell. The
tests that assert what only a POSIX system answers -- a path under `/proc`
or `/dev/fd`, permission bits, FIFOs, `/etc/passwd`, Linux's `O_` flags --
are listed in `win32.mk` (`TEST_SKIP`, each group with its reason) and are
not run there; the tests themselves carry nothing for Windows. CI does not
run the corpus on Windows; whoever changes `lib/win32/` runs it, and adds a
test to the list when it asserts something only POSIX can answer.

`make ext-test` runs here as well: its shared objects are DLLs, which its
hosts load through the shim's `dlopen` (they link `PLATFORM_OBJ`).
