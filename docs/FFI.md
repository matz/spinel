# Spinel FFI

Call C functions from Spinel Ruby programs. No extension compiler, no
`require "ffi"`: declarations go straight into the source and the AOT
compiler generates direct C call sites with the right externs and
linker flags.

## Example

```ruby
module LibC
  ffi_func :strlen, [:str], :size_t
  ffi_func :getpid, [],     :int
end

puts LibC.strlen("hello, world")   # 12
puts LibC.getpid
```

Compile and run:

```sh
./spinel prog.rb && ./prog
```

`libc` and `libm` are always linked; anything else needs `ffi_lib`.

## DSL reference

All FFI declarations go inside a `module` body. The module name becomes
the namespace for the functions (`RAY.InitWindow`, `LibC.strlen`, …).

### `ffi_lib "name"`

Declares that this module needs `-lname` on the link command line. May
appear multiple times per module.

```ruby
module SQL
  ffi_lib "sqlite3"
end
```

### `ffi_cflags "..."`

Declares cflags (include dirs, defines, link-search paths) needed for
this module's externs. Rarely needed -- externs use standard C types
only, so headers don't have to be included in the generated code -- but
useful when a library is installed somewhere non-standard.

```ruby
ffi_cflags "-I/usr/local/include"
ffi_cflags "-Wl,-rpath,/usr/local/lib"
```

### `ffi_source "..."`

Embeds a compile-time C source fragment directly into Spinel's generated
translation unit. This is intended for small adapters that should travel in a
single Ruby source file; normal package C should still use carried `.c` files so
it can be compiled and cached independently.

```ruby
module Tiny
  ffi_source <<~C
    #include <stdint.h>
    intptr_t tiny_double(intptr_t n) { return n * 2; }
  C
  ffi_func :tiny_double, [:long], :long
end
```

The argument must be a compile-time string (a literal/heredoc, adjacent
literals, `String#+`, `__dir__`, or `File.expand_path` over those forms). The
fragment is emitted after `spinel_rt.h` and FFI declarations but before the
generated program's function bodies. Use `ffi_cflags` and `ffi_lib` for any
headers and libraries it needs. Because this is native C, it has the same trust
and portability implications as a package-carried `.c` file.

### `ffi_func :name, [arg_types], ret_type`

Declares a C function callable as `Module.name(...)`.

```ruby
ffi_func :sqlite3_open,        [:str, :ptr],                   :int
ffi_func :sqlite3_close,       [:ptr],                         :int
ffi_func :sqlite3_exec,        [:ptr, :str, :ptr, :ptr, :ptr], :int
ffi_func :sqlite3_errmsg,      [:ptr],                         :str
```

Recognized type specs:

| spec | C type | Spinel type |
|---|---|---|
| `:int` | `int` | `int` |
| `:uint32` | `uint32_t` | `int` |
| `:int32` | `int32_t` | `int` |
| `:uint16` | `uint16_t` | `int` |
| `:int16` | `int16_t` | `int` |
| `:uint8` | `uint8_t` | `int` |
| `:int8` | `int8_t` | `int` |
| `:size_t` | `size_t` | `int` |
| `:long` | `long` | `int` |
| `:float` | `float` | `float` |
| `:double` | `double` | `float` |
| `:bool` | `int` | `bool` |
| `:str` | `const char *` | `string` (NUL-terminated) |
| `:binstr` | `const char *` | `string` (binary-safe, return only) |
| `:ptr` | `void *` | `ptr`; an argument also takes an `IO::Buffer` (its base address) |
| `:float_array` | `const double *` | `Array<Float>` (`.data` pointer) |
| `:int_array` | `const int64_t *` | `Array<Int>` (`.data` pointer) |
| `:void` | `void` | `void` (return only) |

All integer types collapse to `sp_int` (int64) inside Spinel and are
cast to the declared C type at the call boundary. Floats collapse to
`double` the same way. A nullable Integer passed where a C integer type
is declared goes through `sp_oint_arg`, which raises the
`no implicit conversion from nil to integer` TypeError for nil and passes
every other value, `INT64_MIN` included.

The other direction, the C contract header `--ext-entry` emits for a
kernel's exported methods (see [spin.md](spin.md)), spells a nullable
Integer or Float -- a parameter the kernel is also called with `nil`, a
return that can be `nil` -- as `sp_oint` / `sp_ofloat` rather than
`sp_int` / `sp_float`: a struct of the machine word and a nil flag
(`{ sp_int v; sp_bool nil; }`), built with `sp_oint_of(v)` or
`sp_oint_nil()` and read through `.nil` and `.v`. No bit pattern of the
word means nil, so `INT64_MIN` and a NaN of any payload cross the
boundary as values. A parameter or return the analysis never sees `nil`
reach stays a plain `sp_int` / `sp_float`.

The function's `extern` is built from these C types and declared under a
private name that an asm label binds to the real symbol, so it can't
conflict with a header that declares the same function with other types.
`fopen`, which `<stdio.h>` declares returning `FILE *`, can be bound as
`ffi_func :fopen, %i[str str], :ptr`; so can a function whose own header
an `ffi_source` fragment includes. A variadic function with fixed
arguments is declared the same way, with a trailing `...`.

The label names the raw symbol, the one `dlsym` would find for the ffi
gem, not a name a header redirects it to: glibc's `fopen64` under
`_FILE_OFFSET_BITS=64`, `__isoc99_sscanf`, or a `_FORTIFY_SOURCE`
`__*_chk` wrapper. The specs describe that raw symbol's ABI. To call the
redirected variant, name it with the ffi gem's rename form,
`attach_function :fopen, :fopen64, [:string, :string], :pointer`.

`:str` builds the result String by `strlen`, so it stops at the first
embedded NUL. `:binstr` is a return-only variant that builds a
binary-safe String of an exact byte count instead (it reads
`sp_ffi_bin_len`, which the callee sets to the exact byte count
just before returning), so embedded NUL bytes are preserved. Use it for
binary socket reads or raw digests where `:str` would truncate.

`:float_array` / `:int_array` hand the C side a pointer to the Spinel
Array's contiguous storage (`.data`). Length is **not** part of the
spec -- pass it as a separate `:size_t` arg, same way as `:str` +
`strlen`. Lifetime is call-duration only: the GC may free the
underlying Array after the call returns, so the C side must not
stash the pointer (copy if it needs to).

A trailing `blocking: true` (the ffi gem's keyword) marks a call that may
take a while and touches no Ruby object -- a database step, a blocking
read, a sleep:

```ruby
ffi_func :sqlite3_step, [:ptr], :int, blocking: true
```

In a threaded program the worker leaves the world for such a call: its
roots are published on the way out, so a garbage collection raised while
the call runs does not wait for it to return (otherwise every other worker
waits at the barrier until it does), and a collection in progress is
waited out on the way back. The arguments are evaluated before the call
leaves. The bracket costs a scheduler lock round trip, so it is for calls
that block, not for every call; a callback-taking or variadic function
keeps the plain call. The single-threaded runtime ignores the keyword.

### Passing an `IO::Buffer`

An `IO::Buffer` passed where the spec says `:ptr` (or the ffi gem's
`:pointer`, `:buffer_in`, `:buffer_out`, `:buffer_inout`) hands C the
buffer's base address. That is how to build a buffer of 8-, 16- or 32-bit
values in Ruby, with `set_value` / `set_values`, and give it to C, or let C
fill one and read it back with `get_value`:

```ruby
module SDL
  ffi_lib "SDL2"
  ffi_func :SDL_UpdateTexture, [:ptr, :ptr, :buffer_in, :int], :int
  ffi_func :SDL_QueueAudio,    [:uint32, :buffer_in, :uint32], :int
end

pixels = IO::Buffer.new(320 * 200 * 4)
pixels.set_value(:u32, (10 * 320 + 20) * 4, 0xff00ff00)   # x 20, y 10
SDL.SDL_UpdateTexture(texture, nil, pixels, 320 * 4)

samples = IO::Buffer.new(735)
samples.set_values([:U8] * 3, 0, [128, 140, 152])
SDL.SDL_QueueAudio(device, samples, samples.size)
```

The address is taken when the call is made, after every argument has been
evaluated, so it reflects a `resize` and a slice's offset into its source.
It follows CRuby's `rb_io_buffer_get_bytes_for_reading` /
`_for_writing`:

- A freed or zero-size buffer (a null buffer) passes `NULL`.
- A slice whose source was freed or shrunk under it raises
  `IO::Buffer::InvalidatedError`.
- A read-only buffer (`IO::Buffer.for(string)`, a `READONLY` mapping, a
  slice of either) raises `IO::Buffer::AccessError` in any slot except
  `:buffer_in`, which is the one that promises C only reads.

A buffer held in a boxed value, such as an element of a mixed Array, is
recognised at run time. An instance of a user-defined class in a pointer
slot is refused while compiling, since it has no address C can use. The length is not
part of the spec: pass `buf.size` (or a count) as a separate argument.

Lifetime is call-duration only, as for `:int_array`: the buffer is kept
alive across the call, including a `blocking: true` call during which
other threads collect, but the C side must not keep the pointer. During a
`blocking: true` call the buffer, and a slice's source, are locked: another
thread that frees or resizes it gets `IO::Buffer::LockedError` instead of
releasing memory C is still using. A zero-size buffer or slice passes
`NULL`, and an instance of a user class held in a boxed value raises
`TypeError` rather than passing its address.

Nothing checks the length C uses. C sees only the base address, so a
function that reads or writes past `buf.size` (because the count passed
beside it is wrong, or because it assumes a larger buffer) overruns the
buffer, exactly as with `:ptr`, `:int_array` or the ffi gem. Pass the
buffer's own `size` (or a count derived from it) as the length argument.

### `ffi_const :NAME, <int>`

Declares an integer constant accessible as `Module::NAME`. Pure
convenience -- the value is inlined at use sites like any other Ruby
integer constant.

```ruby
ffi_const :SQLITE_OK,   0
ffi_const :SQLITE_ROW,  100
ffi_const :SQLITE_DONE, 101
```

### `ffi_buffer :name, <size>`

Declares a static `size`-byte buffer, accessible as `Module.name`
returning a `:ptr`. Useful as scratch space or as an out-parameter for
functions like sqlite3's `sqlite3_open`, which writes the database
handle into a caller-supplied `sqlite3 **`.

```ruby
ffi_buffer :db_out, 8
SQL.sqlite3_open(":memory:", SQL.db_out)
db = SQL.read_ptr(SQL.db_out)  # the actual sqlite3 *
```

Lifetime: static. The buffer lives for the whole program.

### `ffi_read_<width> :name, <offset>` / `ffi_read_ptr`

Declares a field reader: `Module.name(buf)` returns the value at
`offset` bytes into `buf`. Handy for poking into C structs when you
only need a few fields, or for reading back what a C function wrote
into a buffer you handed it.

The width suffix is one of `u8`, `u16`, `u32`, `u64`, `i8`, `i16`,
`i32`, `i64`, and it is the width of the load: `ffi_read_u8` reads one
byte, not four. A signed suffix sign-extends. `ffi_read_ptr` reads a
`void *`. Any other suffix is refused at the call site rather than
guessed at.

`ffi_write_<width> :name, <offset>` / `ffi_write_ptr` is the mirror
image: `Module.name(buf, val)` stores `val` at `offset` and returns it,
with the same suffixes and the same widths.

Applied to a declared `ffi_buffer`, the access is bounds-checked while
compiling: `offset + width` past the buffer's size is refused at the call
site, since both terms are known there. A pointer from anywhere else -- a
C return value, a callback parameter, a local holding a `:ptr` -- carries
no size, so it stays unchecked and is yours to get right.

```ruby
# sqlite3_open(path, ppDb) writes the new db handle into *ppDb.
# Pull the pointer out of our scratch buffer at offset 0.
ffi_read_ptr :read_ptr, 0

db = SQL.read_ptr(SQL.db_out)
```

### `ffi_struct :Name, [[:field, :spec], ...]`

Declares a named C struct and generates its accessors: `Module.Name_new`
allocates one and returns a boxed pointer, `Module.Name_get_<field>(ptr)`
reads a field and `Module.Name_set_<field>(ptr, val)` writes one. The C
compiler owns the layout, so the accessors are plain member access and the
offsets are whatever the target ABI says -- unlike `ffi_buffer` +
`ffi_read_*`, where you supply the offsets yourself.

```ruby
module M
  ffi_struct :Point, [[:x, :long], [:y, :long]]
end

pt = M.Point_new
M.Point_set_x(pt, 3)
puts M.Point_get_x(pt)     # => 3
```

A struct pointer is a `:ptr`, so it can be handed to any `ffi_func`
argument declared that way -- which is how a C function fills one in.

### `ffi_callback :name, [arg_types], ret_type`

Declares a C function-pointer type usable as an `ffi_func` argument spec.
Passing `method(:some_method)` to an argument of that type compiles a
trampoline that converts the C arguments, calls the method, and converts the
result back, so a Ruby method can be handed to a C API that takes a callback.

```ruby
module L
  ffi_callback :cmp,   [:ptr, :ptr], :int
  ffi_func     :qsort, [:int_array, :size_t, :size_t, :cmp], :void
  ffi_read_i32 :val,   0
end

def cmp(a, b) = L.val(a) <=> L.val(b)

L.qsort(nums, nums.size, 8, method(:cmp))
```

A function that takes a callback has its `extern` skipped and the header
prototype called directly: the per-argument `const` qualification of such a
prototype (`qsort` takes `void *base`, `bsearch` a `const void *`) is not
something the declaration can reproduce.

## Pointer semantics

`:ptr` maps to C `void *`. Values of this type are **not GC-tracked**:
the Spinel garbage collector never follows them and never frees them.
Foreign memory is the user's responsibility.

Two consequences worth knowing:

1. **Call destroy functions explicitly.** Nothing calls `sqlite3_close`,
   `sqlite3_finalize`, or `free()` for you.
2. **Strings passed into C are only valid for the duration of the
   call.** Spinel strings are GC-managed; if a C function stashes the
   pointer somewhere and the string becomes unreachable afterward, a
   later GC cycle will free it out from under the C code. If you need
   a string to outlive the call, copy it into an `ffi_buffer` first.

`ptr` values compare equal to `nil` when the pointer is NULL:

```ruby
db = SQL.read_ptr(SQL.db_out)
if db == nil
  puts "could not open database"
end
```

## Link-flag plumbing

The codegen emits marker comments into the generated C:

```c
/* SPINEL_LINK: -lsqlite3 */
/* SPINEL_CFLAGS: -I/usr/local/include */
```

The `spinel` compiler scrapes these from the generated C in-process and
appends them to the `cc` invocation. If you want to override (e.g. static
linking or a custom lib path), use `-c` to stop at C and drive the linker
yourself.

## Limitations

Scalars, strings, opaque pointers, integer constants, raw byte buffers,
struct declarations and callbacks are covered. Not supported yet:

- **No variadic C functions** (`printf(...)`). Use Spinel's built-in
  `printf` if you want formatted output.
- **Pointers can't enter polymorphic values.** Don't put a `:ptr` into
  a `poly_array` or a generic `Hash`; keep them as plain locals or
  wrap them in a class with a `ptr`-typed ivar.

## The ffi gem (`require "ffi"`)

The DSL above is compile-time: every name, type and library is written in
the source, and each call is a direct C call. Gems written against CRuby's
`ffi` gem often need more than that -- a library path computed at run
time, `FFI::Struct` layouts, `MemoryPointer`s, callbacks stored and called
later, functions attached under names that come from data. For those,
`require "ffi"` loads the bundled package (`packages/ffi`): the gem's API
in Ruby over `packages/ffi/sp_ffi.c`, which is the libffi and dlopen glue
and nothing else.

- A module that `extend`s `FFI::Library` is then the package's: `ffi_lib`,
  `attach_function`, `callback`, `typedef` and `enum` run when the module
  body runs, as in CRuby, and a symbol that is missing raises
  `FFI::NotFoundError` there.
- A literal `attach_function :name, ...` also gets a method `name` on the
  module (and as an instance method, for a module that is included) at
  compile time, since a method's name has to be known then.
- A call to a function attached under a computed name
  (`SDL.Init(...)`, or a bare `InitWindow(...)` in a class that includes the
  module) goes through the table of attached functions at run time.
- `FFI::Pointer` / `MemoryPointer` / `Buffer`, `FFI::Struct` / `Union`
  (nested, by value and by reference, arrays in layouts), `FFI::Function`,
  enums, typedefs, varargs and `FFI::AutoPointer` are provided.

The package needs the system libffi where spinel is built (like openssl, it
is probed for). Without it, `require "ffi"` stays a no-op and an `extend
FFI::Library` module is read by the compile-time DSL, which accepts the
gem's spellings for the literal cases.

## Fiddle (`require "fiddle"`)

The stdlib's `fiddle` is the other dynamic-FFI API a Ruby program meets, and
`require "fiddle"` loads a bundled package (`packages/fiddle`) that provides
it: `Fiddle::Handle` (and `Handle::DEFAULT` / `NEXT`), `Fiddle::Function`,
`Fiddle::Pointer`, `Fiddle::Closure` / `Closure::BlockCaller`, the `TYPE_*`,
`SIZEOF_*` and `ALIGN_*` constants, and `Fiddle.malloc` / `realloc` / `free`.

```ruby
require "fiddle"
libc = Fiddle.dlopen(nil)
strlen = Fiddle::Function.new(libc["strlen"], [Fiddle::TYPE_VOIDP], Fiddle::TYPE_SIZE_T)
strlen.call("hello")   # => 5
```

It is the ffi package's native layer under Fiddle's API, so it carries no C of
its own and exists where the ffi package does: it needs the system libffi. The
library and the symbol are looked up when the program runs, and a function's
signature is whatever it was made with.

Behaviour that follows Fiddle rather than the ffi gem: a `Pointer` checks no
bounds (`ptr[0, 9]` reads nine bytes whatever `ptr.size` says; size 0 means
unknown), `ptr[i]` is a signed byte, `Pointer#free` is the free function (not
a call of it: that is `call_free`), and a variadic function's types end with
`TYPE_VARIADIC` and are called with a (type, value) pair per variadic argument.
A pointer argument that is not a Pointer, String, Integer, nil or something with
`to_ptr` raises `TypeError` (CRuby's Fiddle crashes there).

`require "fiddle/import"` adds `Fiddle::Importer`: a module that `extend`s it
loads libraries with `dlload`, aliases types with `typealias` and declares C
functions from their prototypes with `extern` (the prototype parser is the
gem's own), and calls them on the module -- `LibC.strlen("hello")`. As with an
`extend FFI::Library` module, the compiler sends a call of a name the module has
no method for to the declared function, and a class that includes the module
(which CRuby's Importer does not support) may call them bare. Like the gem,
`dlload nil` searches nothing; pass `Fiddle.dlopen(nil)` for the process.

Not provided, and so refused when the program is compiled: `Importer#struct`,
`union`, `value` and `bind` (they build classes and methods from data), and
`CStruct`, `Pinned`, `MemoryView`, `Fiddle.dlwrap` / `dlunwrap`.

## Examples

Runnable examples live under `examples/ffi/`:

- `examples/ffi/libm/`     -- libc / libm smoke (cos, sqrt, pow, strlen, getpid)
- `examples/ffi/sqlite/`   -- blog system (posts, tags, comments) on sqlite3

Each subdirectory has a `README.md` with build instructions and the
required system packages.
