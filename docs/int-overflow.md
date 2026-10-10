# Integer overflow (`--int-overflow`)

CRuby's `Integer` is arbitrary precision: it never overflows, it grows. Spinel
compiles `Integer` to a fixed-width machine integer (`sp_int`, the target's
pointer width: **64-bit** on amd64 and arm64, range `-2**63 .. 2**63 - 1`;
32-bit on a 32-bit target, see [below](#the-width-is-the-targets)) because a
machine word is what makes the generated C fast. `--int-overflow=MODE` chooses
what happens when an `Integer` result crosses that boundary. The rest of this
page says 64 bits; read 32 on a 32-bit target.

```sh
spinel app.rb --int-overflow=raise     # default
spinel app.rb --int-overflow=wrap
spinel app.rb --int-overflow=promote
```

## Modes

| mode | on overflow | matches CRuby? | use it for |
|---|---|---|---|
| **`raise`** (default) | raises `RangeError` (`integer overflow in +`) | no -- CRuby would grow the integer | catching overflow loudly; never silently wrong |
| **`wrap`** | two's-complement wraparound, like C (`a + b` with no check) | no | modular arithmetic, hashes, checksums, PRNGs -- anywhere defined wraparound *is* the intent |
| **`promote`** | promotes the result to an arbitrary-precision integer (bigint) | yes | CRuby-faithful integer math (experimental, see below) |

The mode applies to integer `+`, `-`, `*`, unary `-`, and (under `promote`) `**`
and shifts. It does not change division: `1 / 0` is always a
`ZeroDivisionError` regardless of mode.

A shift that lands exactly on the sign bit follows the mode too: `1 << 63`
(a literal count or a run-time one) is `-2**63` under `wrap`, a `RangeError`
under `raise`, and the Bignum `2**63` under `promote`. The value `-2**63`
itself is an ordinary `Integer` in every mode -- reached by `-9223372036854775807 - 1`,
by `~0x7fffffffffffffff`, by a wrapping `+ - *` -- and never reads as `nil`:
the nil of a nullable Integer slot is a flag beside the word, not a bit
pattern of it (see [limitations.md](limitations.md#a-nil-read-out-of-an-integer-container)).

### `raise` (default)

The default refuses to be silently wrong. A computation that exceeds 64 bits is
almost always a bug or a case that needs `promote`; raising surfaces it at the
point it happens rather than producing a truncated value. This is a deliberate
deviation from CRuby (which would never raise here) in favour of loudness.

A Float converted to an `Integer` outside the signed 64-bit range (`1e300.round`,
`1.0e19.to_i`, `floor`, `ceil`) raises `RangeError` (`float out of Integer range
(Bignum promotion pending)`) in every mode but `promote`, which answers the bignum
as CRuby does.

### `wrap`

`wrap` skips the overflow check entirely, so arithmetic is plain C wraparound.
Choose it when wraparound is the algorithm -- hashing, checksums, fixed-width bit
manipulation, RNGs -- not as a blanket "make overflow go away", since it will
silently truncate a value the program genuinely needed. It is the fastest mode
(no checks); for example the optcarrot build uses `wrap`.

### `promote`

`promote` makes integers behave like CRuby's: a result that exceeds 64 bits
becomes a bigint instead of overflowing. Small values stay unboxed machine
integers (like CRuby's fixnum), and only the ones that actually overflow pay the
bigint cost, so it is more practical than widening everything.

`promote` is **experimental**: most integer code works, but coverage is not yet
complete (some overflow paths through method arguments, closures, and certain
containers still raise rather than promote, and very large integer *literals*
are not yet represented). Treat it as opt-in CRuby fidelity, not a finished
guarantee. It also carries a runtime cost (bigint allocation and GC pressure),
so the default stays `raise`.

## The width is the target's

`sp_int` is `intptr_t` (lib/sp_types.h): the Integer is as wide as a pointer
on the machine the program is compiled for. On amd64, arm64 and every other
64-bit target that is 64 bits; on i386 (`cc -m32`) and wasm32 it is 32, with
the range `-2**31 .. 2**31 - 1`, and the overflow modes above apply at that
boundary. Everything that depends on the width follows the target, not the
host the compiler runs on:

- An integer literal past the target's `sp_int` is a Bignum there, as it is in
  a 32-bit CRuby: `0xdeadbeef` or `4_000_000_000` compile to a bigint
  constant for a 32-bit target and to a plain `sp_int` for a 64-bit one.
- The width comes from the C compiler that builds the program. `spinel` uses
  its own (it was built with the same toolchain); with `--cc` naming another
  compiler it asks that one once (`-dM -E`) and classifies literals for its
  pointer width, so `spinel --cc='cc -m32' app.rb` on a 64-bit host produces a
  correct 32-bit program.
- A 32-bit target also gets 64-bit `time_t` and file offsets on glibc
  (`-D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64`) and, on i386, SSE arithmetic
  (`-msse2 -mfpmath=sse`): the x87 unit rounds every intermediate at 80 bits
  and `3.7.round(1)` would come out `3.8`. The driver adds these to the
  program's compile; `common.mk` adds them to the runtime's when `$(CC)` is a
  32-bit compiler (`SPINEL_INT_BITS`).
- `Integer#size` is 4, `Integer#bit_length` and the shift helpers use the
  width, `String#unpack` of a 64-bit directive (`Q`, `q`) boxes a Bignum when
  the value does not fit, and a Bignum read out of a poly slot into an Integer
  is a `RangeError` when it does not fit.
- `INT32_MIN` is an ordinary Integer there, as `INT64_MIN` is on 64-bit: the
  nil of an `Integer | nil` slot is a flag beside the word on both widths.
- Known gap on 32-bit: FFI marshalling of 64-bit C types is not done.

For the developer: `make test-corpus SPINEL_INT_OVERFLOW=wrap` (or `promote`)
runs the test corpus under that mode, and CI runs both on every push to
master. The tests a mode answers otherwise today are listed in
`test/int-overflow/<mode>-skip.txt`; a fix takes its test off the list.

For the developer: `make test-corpus CC='cc -m32'` runs the test corpus as
32-bit programs. Use a separate work tree, or `make clean` first: the runtime
objects and the precompiled header are built for one width. A test that
assumes a 64-bit Integer (values or arithmetic past 2^31, `Integer#size`, a
printed hash, a 64-bit FFI width) says `# spinel: int64` in its first line and
is filtered out there; CI runs that lane on every push.
`SPINEL_INT_OVERFLOW=promote make test` and `SPINEL_INT_OVERFLOW=wrap make
test` run the corpus in the other two modes (each with its own precompiled
header): the tests that pin a raise-mode RangeError stay out of both, the
`promote_*` tests run only in promote, and a test whose answer is the Bignum
the default mode's growth-pattern promotion produced stays out of wrap.

## Using it when you compile the C yourself

In the normal `spinel app.rb` flow the driver compiles and links in one step and
passes the matching `-DSP_INT_OVERFLOW_MODE_{RAISE,WRAP,PROMOTE}` to the C
compiler for you, so `--int-overflow=MODE` is all you need.

If you emit C with `-c` and compile it separately, the generated code and the
runtime must agree on the mode, so pass the same define to your own `cc`:

```sh
spinel app.rb --int-overflow=wrap -c -o app.c
cc app.c -DSP_INT_OVERFLOW_MODE_WRAP -Ilib libspinel_rt.a -lm -o app
```

## See also

- [limitations.md](limitations.md) -- where Spinel's static, fixed-width model
  differs from CRuby, including integer precision.
