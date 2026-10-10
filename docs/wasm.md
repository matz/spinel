# WebAssembly (`--target=wasm32-wasi`)

```sh
spinel --target=wasm32-wasi app.rb        # -> app.wasm
wasmtime run app.wasm
```

`--target=wasm32-wasi` builds the program as a WebAssembly module for a
WASI (preview 1) host: wasmtime, Node's `node:wasi`, a browser with a WASI
shim. The compiler runs on the host as usual; the C it emits is compiled by
the [wasi-sdk](https://github.com/WebAssembly/wasi-sdk)'s clang against a
runtime archive built for the target, and the module is written to
`<name>.wasm` (or `-o`).

## What you need

- The wasi-sdk (34 or later). Point `WASI_SDK` at it, or its own
  `WASI_SDK_PATH`; the default is `/opt/wasi-sdk`, where its installer puts
  it. `--cc` overrides the compiler as on any target.
- The runtime archive for the target, `lib/wasm32-wasi/libspinel_rt.a`, and
  the bundled packages' `packages/*/sp_*_wasi.o`: `make wasm-rt` builds
  them with the same sdk. They are not built by default.
- An engine that implements the exception-handling proposal in its
  standardized (`exnref`) form: wasmtime 48 runs the modules as they are
  (`-W exceptions=y` is its switch), and the current Chrome, Firefox,
  Safari and Node implement it too. Ruby's `raise`/`rescue` is C
  `setjmp`/`longjmp`, which the wasi-sdk lowers onto those instructions
  (`-mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false`; the driver
  passes them).

## What is different on this target

- **`Integer` is 32-bit.** `sp_int` is the pointer width
  ([int-overflow.md](int-overflow.md)), and wasm32 has 32-bit pointers, so
  the Integer range is `-2**31 .. 2**31 - 1`, `2**40` is a `RangeError`
  under the default `--int-overflow=raise`, and an integer literal past that
  is a Bignum. The same program builds for a 64-bit host with a 64-bit
  Integer; a test that assumes 64 bits says `# spinel: int64` in its first
  line.
- **No `Fiber`, `Enumerator#next`, `Thread`.** Core wasm has no stack
  switching, so `Fiber.new` raises `FiberError` (failed to allocate fiber
  stack), and everything built on it (external enumerators, `Enumerator.new`
  with a `Yielder`, `loop` over `#next`) fails the same way. A program that
  uses `Thread`, `Mutex`, `Queue` or `ConditionVariable` is refused at
  compile time: wasm threads need SharedArrayBuffer and a threaded runtime
  archive, and neither is provided.
- **No processes, signals, name resolution or user database.** WASI has
  none: `fork`, `spawn`, backticks, `system`, `Process.kill`, `trap`,
  `Socket.getaddrinfo`, `Etc`, `Process.uid` answer as an unsupported call
  does on any POSIX system (`Errno::ENOSYS`, `Errno::ENOTSUP`,
  `SocketError`), never as a link error. Sockets cannot be opened in
  preview 1 (`Errno::ENOTSUP`). Files work within the directories the host
  grants (`wasmtime run --dir=.`).
- **FFI binds only what is linked in.** There is no `dlopen` on WASI, so
  an `ffi_lib` names nothing; a C function the program declares has to be
  in the module (a package's carried C, or `--link` of an object built by
  the same sdk), and its declared types have to be the ones wasi-libc
  gives it.
- **`Random.urandom` and `SecureRandom`** read the host's entropy through
  WASI's `random_get`.
- **The stack.** The main thread's stack is 8 MB of linear memory, the
  native default. wasmtime also caps the *native* stack a module's frames
  may use at 512 KB, which a deeply recursive program reaches long before
  its own 8 MB; raise it with `wasmtime run -W max-wasm-stack=16777216`.
- **The GC.** The runtime's slab allocator takes 64 MB of linear memory
  once and gives none of it back (wasm memory only grows); past that every
  block is a `malloc`. `SPINEL_GC_SLAB_MB=N` in the module's environment
  takes N MB instead, rounded down to a multiple of 4; below 4, or a value
  that is not a number, the slab is off, as with `SPINEL_GC_SLAB=0`. The
  size matters where a host counts the whole memory against a limit,
  touched or not: the slab's size is added to the memory the module needs
  with the slab off, so a module of 13 MB with the slab off is 77 MB with
  the default and 21 MB with `SPINEL_GC_SLAB_MB=8`. The cost shows when the
  live heap outgrows the slab: an allocation then tries the slab before its
  `malloc`, and a program allocating 150 MB ran 1.5 times as long with 8 or
  16 MB as with 64, and about as long with the slab off as with 64. Size it
  to the heap, or turn it off.
- **Function size.** wasm engines cap a single function's body (wasmtime:
  7.6 MB). The `setjmp` lowering keeps every live local in memory around
  every call in a function that contains a `rescue`, so a very large
  top-level program with many `begin`/`rescue` blocks and hundreds of
  locals can exceed the cap in `main` (the engine says
  `function body size count exceeds limit`). A method is a function of
  its own: moving such code into methods is the fix.

## The pieces

- `lib/wasi/` is the shim: stand-ins for the POSIX headers wasi-libc leaves
  out (`sys/wait.h`, `netdb.h`, `pwd.h`, `ucontext.h`, and additions to
  `signal.h`, `unistd.h`, `sys/socket.h`, `sys/resource.h`, `sys/ioctl.h`,
  `time.h`, `stdio.h` through `#include_next`) and `sp_wasi.c`, which
  defines the calls to fail with `errno`. Only a wasi build sees the
  directory (`-Ilib/wasi`).
- `src/main.c` picks the compiler, the archive, the include path, the
  emulation defines (`_WASI_EMULATED_SIGNAL`, `_WASI_EMULATED_MMAN`,
  `_WASI_EMULATED_PROCESS_CLOCKS`, `_WASI_EMULATED_GETPID`), the sjlj
  flags, the stack size and the emulation libraries, and records them all
  in `--print-build`. The Makefile's `wasm-rt` target compiles the runtime
  with the same list (`WASI_CFLAGS`).
- `spinel -c --target=wasm32-wasi` (or `--cc=<wasi clang>`) classifies
  integer literals for a 32-bit target even when it only emits C.
