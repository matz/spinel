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
