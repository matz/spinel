# Spinel Compiler Fixes — socket package bring-up

These are compiler (C frontend in `src/`) changes made while getting the
`packages/socket` native (Path B) binding to compile and link. They are
**general compiler bug fixes**, not socket-specific; any PR should describe
them as such. Companion design notes live in `SOCKET_IMPL.md`.

## Context: how Path B native classes work

A native package (`packages/socket/socket.rb`) declares classes with
`native_struct "Name", "sp_CStruct"[, "free_sym"]`, binds constructors with
`native_new [specs], "csym"` and instance methods with
`native_method :name, [specs], ret, "csym"`. Several such classes may share
ONE backing C struct (every socket class reuses `sp_Socket`). The compiler
tags each as `ClassInfo.is_native_class`, records `native_methods[]` keyed by
`class_id`, and codegen emits direct C calls to the declared symbols.

---

## Fix 1 — `register_ffi_decls` only tagged the FIRST native class
**File:** `src/analyze_scope.c`
**Function:** `register_ffi_decls` (starts L1918)
**Pre-scan loop:** L1948-L1973 (the `native_struct` pre-scan)

The pre-scan that tags each `native_struct` class as `is_native_class` had a
`break` after the first qualifying statement, so only `BasicSocket` got tagged.
The other 7 socket classes were treated as *regular* user classes, which (a)
caused codegen to ALSO emit a `static sp_X *sp_X_new(void)` for each (clashing
with the `native_new` externs: `error: conflicting types for sp_Socket_new`),
and (b) meant `emit_native_ctor` / the native dispatch never fired.

**Fix:** removed the `break` (kept the existing explanatory comment). Every
`native_struct` in the module is now tagged. Native classes that are later also
declared as `class X < Y` (e.g. `class TCPSocket < IPSocket` at socket.rb L326)
still merge into the same `ClassInfo` because `comp_class_index` finds the
pre-registered class by name.

**Verified:** plain `make` now builds; `TCPSocket.new(...)` reaches codegen as a
native ctor.

---

## Fix 2 — `native_new`/`native_method` bound to the WRONG class_id
**File:** `src/analyze_scope.c`
**Pre-scan** sets a module-local `int native_cid` (L1947) to the LAST
`native_struct` it saw. The SECOND loop (L1974+, which processes
`native_new`/`native_method`) still used that stale `native_cid`, so every
binding landed on `UNIXServer` (the last native_struct) instead of its own
class. Result: `comp_native_method_find(c, 5 /*TCPSocket*/, "new", 2, ...)`
returned -1 → `.new` fell through to the generic `sp_TCPSocket_new()` with 0
args → "too few arguments".

**Fix:** in the second loop, re-resolve `native_cid` from the class name
whenever we hit a `native_struct` (mirroring the pre-scan). Replaced the bare
`if (sp_streq(dname, "native_struct")) continue;` (L2051) with code that does:
```c
if (sp_streq(dname, "native_struct")) {
  if (an >= 2) {
    const char *clsname2 = ffi_arg_str(nt, args[0]);
    if (clsname2) {
      int ex2 = comp_class_index(c, clsname2);
      if (ex2 >= 0) native_cid = ex2;
    }
  }
  continue;
}
```
Now `c->native_methods[]` correctly carries `class_id` per class (e.g.
`sp_TCPSocket_new` is `cid=5 kind=1 nargs=2`).

---

## Fix 3 — missing `typedef` alias for shared-struct native classes
**File:** `src/codegen.c`
**Native forward-decl loop:** L4948-L4955

When several native classes share one backing C struct (`sp_Socket`), the
forward-decl loop only emitted `typedef struct sp_Socket_s sp_Socket;`. The
per-class alias `typedef struct sp_Socket_s TCPSocket;` etc. was missing, even
though the generated native externs and `sp_<CName>` user-method forward decls
reference `TCPSocket`, `sp_TCPSocket`, `sp_IPSocket`, `sp_TCPServer`, etc.
→ `error: unknown type name 'sp_TCPSocket'`.

**Fix:** in the loop, emit a per-class alias whenever `strcmp(nm, cs)` (the
class's `c_name` differs from the backing struct); also synthesize the
`sp_<Name>` alias (used by user-method forward decls for native classes) so
`sp_TCPSocket_accept`, `sp_IPSocket_addr`, etc. resolve. Current code:
```c
const char *cs = cf->classes[nci].c_struct;     // "sp_Socket"
const char *nm = cf->classes[nci].c_name;       // e.g. "TCPSocket" / "sp_TCPSocket"
buf_printf(&b, "typedef struct %s_s %s;\n", cs, cs);
if (strcmp(nm, cs))
  buf_printf(&b, "typedef struct %s_s %s;\n", cs, nm);
char ubuf[64];
snprintf(ubuf, sizeof ubuf, "sp_%s", cf->classes[nci].name);
if (strcmp(ubuf, cs) && strcmp(ubuf, nm))
  buf_printf(&b, "typedef struct %s_s %s;\n", cs, ubuf);
```
NOTE: `c_name` for a plain class named `Socket` is `Socket` (no `sp_` prefix);
the `sp_<Name>` alias is what user-method forward decls emit. Both are needed.

---

## Fix 4 — duplicate symbol when a method is BOTH user-defined and native
**File:** `packages/socket/socket.rb` (package source, not the compiler)

`BasicSocket` declared `sendmsg`/`recvmsg`/`local_address`/`remote_address`
both as `native_method` (so `self`-calls inside the class work — a user method
on a native class cannot call a native method on `self`) AND as a Ruby wrapper
(e.g. `def local_address; Addrinfo.new(self.getsockname); end`). Codegen then
emits a user forward-decl `sp_BasicSocket_local_address(BasicSocket*)` AND a
native extern `sp_BasicSocket_local_address(sp_Socket*)` →
`error: conflicting types for 'sp_BasicSocket_local_address'`.

**Fix:** removed the Ruby wrappers for those four methods in `BasicSocket`
(socket.rb L263-L270). They remain native methods, and callers that need an
`Addrinfo` use `Addrinfo.new(self.getsockname)` / `getpeername` directly.
`sendmsg`/`recvmsg` keep only the `*_nonblock` wrappers (which delegate to the
native `sendmsg`/`recvmsg`).

(This is a package-side collision, not a compiler bug. It only manifests once
Fix 1/2 correctly tag the classes as native so the native externs are emitted.)

---

## Debug output left in the tree (REMOVE before PR)
- `src/codegen_call.c` `emit_native_ctor` (L3531): a temporary `fprintf(stderr,
  "[enc] ...")` was added during diagnosis and then REMOVED. Confirm no
  `[enc]`/`[ffi]`/`[cg]` prints remain in `src/*.c` (verified clean).

## Build note
Use `bash -c 'make -j2'` — the top-level `Makefile` contains a bash-ism
(`${var//...}`) that fails under `dash`/`sh` with "Bad substitution". The 2-core
box builds faster with `-j2`.

## Linking a final binary by hand (for testing without the test harness)
```
cc -I. -Ilib -Ipackages/socket <app>.c \
   $(ls build/sp_*.o) packages/socket/sp_socket.o \
   lib/libspinel_rt.a -lm -o <app>
```
(`build/sp_*.o` are the runtime objects; `lib/libspinel_rt.a` is `SP_RT_LIB`
and provides regexp/bigint/etc. — linking the loose `build/sp_*.o` WITHOUT
`lib/libspinel_rt.a` gives `undefined reference to re_*`.)

---

## Fix 5 — `native_func` (Path B) poly args not coerced for `:int`/`:float`/`:bool`
**File:** `src/codegen_call.c`
**Function:** the `native_func` dispatch block (≈L12218, after the JSON.parse
special-case)

The `native_method`/`native_new` emit paths already coerce a `TY_POLY` actual to
the declared scalar spec (`sp_poly_to_i`/`sp_poly_to_f`/`sp_poly_to_s`), but the
`native_func` (module-level `Module.func(...)`) path only coerced `:string` poly
args — `:int`/`:float`/`:bool` polys were emitted raw as `sp_RbVal`, so e.g.
`Socket.sockaddr_in(port, host)` (where `port` is a runtime int) compiled to a
call with a `sp_RbVal` where the C symbol expects `mrb_int` →
`incompatible type for argument 2 of 'sp_Socket_pack_in_wrap'`.

**Fix:** in the `native_func` arg loop, added the same `TY_POLY` coercions as the
`native_method` path:
```c
else if (sp_streq(spec, "int")   && at == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[ai], b); buf_puts(b, ")"); }
else if (sp_streq(spec, "float") && at == TY_POLY) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, argv[ai], b); buf_puts(b, ")"); }
else if (sp_streq(spec, "bool")  && at == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[ai], b); buf_puts(b, ")"); }
```

---

## Package-side bugs fixed during bring-up (recorded in `SOCKET_IMPL.md` §12)

These are `packages/socket/sp_socket.{c,h}` and `socket.rb` correctness fixes,
not compiler bugs:

- **Native method arg count must equal the C signature.** `bind`/`connect`/
  `sendto`/`recv`/`send`/`sendmsg`/`recvmsg`/`setsockopt` had trailing C params
  (`len`, `flags`) not declared in the `native_method` spec; the call site passed
  only the declared args, leaving the extras uninitialized (garbage). `recv` got
  `flags=44` (a `MSG_TRUNC`-like flag) which zeroed the returned buffer; `bind`
  got a garbage `len` → `EINVAL`. Fixed by dropping the extra params and deriving
  `len` from `sp_str_byte_len(sa)` inside C. (Header prototypes updated to match.)
- **`close` on a UDP socket raised `ENOTCONN`.** `shutdown(SHUT_RDWR)` fails for
  an unconnected datagram socket; added `sp_socket_shutdown_quiet` (ignores
  errors) and `close` now uses it.
- **Ruby method name colliding with a native `csym`.** A Ruby `def recvfrom` on
  `BasicSocket` compiles to `sp_BasicSocket_recvfrom`, clashing with the native
  method's `csym` (`conflicting types`). Renamed the native `csym`s to
  `sp_BasicSocket_recvfrom_raw`/`recvmsg_raw`; the Ruby wrappers `recvfrom`/
  `recvmsg` call the `_raw` natives and wrap the binary sockaddr in a pure-Ruby
  `Addrinfo`. (The compiler generates a C symbol `sp_<Class>_<method>` for a Ruby
  method of that name, so any native method needing a Ruby wrapper must use a
  `csym` distinct from that form.)
- **`Addrinfo` is pure-Ruby; added `def family; @family; end`.** The compiler
  returns `nil` (instead of raising) for an undefined method, so a missing
  accessor silently yielded `nil`. Also: `:STREAM`/`:DGRAM`/`:RAW`/`:TCP`/`:UDP`
  symbols are mapped to their `SOCK_*`/`IPPROTO_*` ints in `Socket.getaddrinfo`
  and `Addrinfo.getaddrinfo` (the native `getaddrinfo` wrapper expects ints).
- **`UDPSocket#bind`/`#connect`/`#send(host,port)` sugar omitted** (compiler
  mis-routes the 1-arg `self.bind(sockaddr)` into the 2-arg Ruby method). Callers
  pass a sockaddr string to the inherited `BasicSocket#bind`/`#connect`.
