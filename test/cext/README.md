# Restricted C extension layer

The first stage of #7210 supplies the immediate value representation and
standalone headers. It does not load CRuby binaries or enable requiring C
extensions yet. Include these headers explicitly with `-Iinclude`; ordinary
Spinel builds continue using the existing runtime.

`make cext-header-test` checks pointer width, signed Fixnum boundaries (with
UBSan), nil/boolean truthiness, Symbol round trips, handle discrimination,
and compile-time diagnostics for unsupported object layout access and eval.

Fixnum conversion macros take values in `RUBY_FIXNUM_MIN..RUBY_FIXNUM_MAX`;
heap integer conversions belong to the runtime layer. `VALUE` uses unsigned
shifts so negative immediate encoding has no signed-shift undefined behaviour.
Symbols are static IDs; the later source scanner must restrict computed method
IDs before enabling extension dispatch.

`ruby.h` brings in what CRuby's does for every extension: `_GNU_SOURCE` /
`_DARWIN_C_SOURCE` ahead of any system header (so a `-std=c11` build still
declares `clock_gettime` and `getpid`), `<stdbool.h>`, `<time.h>`,
`<sys/types.h>`, `<unistd.h>` and `rb_pid_t`.

The encoding and thread headers currently declare no operations. `rb_io_t`
contains only `fd` and `mode`; obtaining it from a Ruby IO belongs to the IO API
stage. Unsupported layout access emits a compiler error on GCC and Clang;
other compilers still refuse at link time rather than providing an ABI.

The second stage adds `lib/sp_cext.h` and `make cext-runtime`, an explicitly
selected `lib/libspinel_cext_rt.a`. Only this archive compiles the collector
with `SP_CEXT`; the regular runtime contains neither extension hooks nor their
branches. A C caller enters an arena before converting values, then restores
its checkpoint on return or exceptional unwind. Checkpoints include the arena
depth, so jumping through several nested C calls discards all skipped pins.

The open-addressed table is weak. Arenas, registered `VALUE *` addresses and
`TypedData` mark callbacks root handles. A live Ruby object preserves its
canonical handle even after a C call ends, while dead handles are removed
before the collector can recycle object addresses. Floats are keyed by their
bit patterns; large native integers use handles rather than truncating the
Fixnum representation. Raw string identity is supported here; the compiler's
future boundary stage still must promote strings to shared mutable handles.

Data-class lifecycle helpers provide the prefix, mark/finalize functions and
type-parent checks for the compiler to use later. Non-WB-protected data classes
are pinned remembered; the collector calls `dmark` on minor collections even
when a C store had no write barrier. This stage does not yet provide public
class lookup, allocation macros, send dispatch or threaded extension access.

`make cext-gc-test` checks identity, table resizing, nested arena unwind,
registered-address deduplication/unregistration, weak reclamation, strings,
`TypedData` children and finalization. It repeats under generational verification,
the list allocator and allocation stress.

The third stage adds exception APIs over the real generated-runtime handler
stack: `rb_raise`, exception construction/re-raise, `rb_protect`, `rb_ensure`,
`rb_rescue{,2}`, `rb_jump_tag`, error state, and errno failures. Call
`sp_cext_exceptions_init()` before extension initialization. Exception-frame
checkpoints restore skipped arenas even when a C raise lands in a normal Ruby
handler. The opt-in fiber context carries its arena, error state and handler
checkpoints; pinned handles in suspended contexts remain GC roots.

The future compiler bridge must install `sp_cext_exception_new_fn` to allocate
the full struct and scanner for a user exception class. Without it subclass
construction reports the missing allocator, instead of treating a base
exception allocation as a struct with additional Ruby ivars. The host test
uses an actual generated subclass to check the contract and object identity.

`make cext-exceptions-test cext-exceptions-oracle` runs the shared C API checks
against both runtimes, plus Spinel-specific tests for C/Ruby raises, nested
arena unwind, cause/backtrace capture, subclass layout, saved fiber contexts,
GC stress and fatal termination. Unlike `rb_bug`, `rb_fatal` runs ensures
while bypassing rescues, as documented in
[CRuby's extension guide](https://docs.ruby-lang.org/en/master/extension_rdoc.html).
The compiler bridge, literal eval, VALUE-aware formatting, send dispatch and
threaded extension execution remain for the later steps.

Step 4 adds a standalone definition recorder and a versioned manifest plus
literal Ruby declaration artifacts. See [the recorder contract](../../docs/internals/cext-recorder.md)
for its restricted initialization API, build command, import/literal checks
and the later compiler bridge. `make cext-recorder-test cext-recorder-oracle`
checks deterministic output and compares the same C fixture against CRuby.

Step 5 adds the compiler bridge and an end-to-end CRuby oracle:
`make cext-compiler-test`. Generated declarations can be required and the exact
retained object archive passed with `--link`. Native methods use the ordinary
call plan, data objects carry their Ruby ivars, and boundary sharing is decided
by analysis.

The `symbol_names` fixture checks native `rb_sym2str` frozen state and
identity, including empty, dynamically interned and embedded-NUL names,
across collections. It also checks explicitly typed Symbol name readers
and separation from same-content String literals. `Symbol#to_s` keeps its documented distinct frozen
String behavior.

The later steps (sqlite3 and racc/cparse end to end, #7210) are listed at
the end of [the recorder contract](../../docs/internals/cext-recorder.md):
hidden instance variables, spread sends, `Class.new` during Init, the
C API sqlite3 needs (`rb_scan_args`, the number conversions and
`rb_integer_pack`, the encodings, `StringValue`, `rb_path2class`, ...), and
the bundled packages `packages/sqlite3` and `packages/racc`, whose
extensions `make` builds (`make bundled-cext`) and whose tests the gate runs
with the other packages'. Their CRuby oracle cases are
`test/cext/compiler/{hidden_ivars,raise_program_class,conversions,spread_sends}`.
