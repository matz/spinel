# SOCKET_IMPL.md — Implementing the `socket` standard library in Spinel

This document is the **scouting report** for implementing Ruby's `socket`
module (the `RubyVM`/CRuby `ext/socket`) as a Spinel bundled package, so that
`require "socket"` yields an API matching the real `Socket`, `BasicSocket`,
`IPSocket`, `TCPSocket`, `TCPServer`, `UDPSocket`, `UNIXSocket`, `UNIXServer`,
`Addrinfo`, `Socket::Option`, `Socket::Ifaddr`, `Socket::AncillaryData`, etc.

It records everything needed to do the implementation pass: file locations,
the Spinel native-binding mechanism (`native_lib` / `native_struct` /
`native_new` / `native_method`), the runtime ABI, the existing `sp_net`
POSIX primitives we can reuse, what is missing from them, and the exact
CRuby source (`/home/user/ruby/ext/socket`) the behavior must mirror.

---

## 1. Where things live

### CRuby socket module (the oracle / reference behavior)
- `/home/user/ruby/ext/socket/lib/socket.rb` — the bulk of the public API
  (`Addrinfo`, `BasicSocket`, `Socket` class methods like `Socket.tcp`,
  `Socket.tcp_server_loop`, `Socket.udp_server_loop`, `UDPSocket`, `TCPServer`,
  `UNIXServer`, `UDPSource`, etc.). **Start here for the user-facing API.**
- `/home/user/ruby/ext/socket/*.c` — the C extension that backs the classes:
  - `socket.c`      — `Socket` class: `initialize`, `connect`, `bind`,
                     `listen`, `accept`, `sysaccept`, `recvfrom`,
                     `socketpair`/`pair`, `gethostname`, `getservbyname`,
                     `getservbyport`, `getaddrinfo`, `getnameinfo`,
                     `sockaddr_in`/`unpack_sockaddr_in`, `sockaddr_un`,
                     `ip_address_list`, `tcp_fast_fallback`.
  - `basicsocket.c` — `BasicSocket`: `for_fd`, `close_read`, `close_write`,
                     `shutdown`, `setsockopt`, `getsockopt`, `getsockname`,
                     `getpeername`, `getpeereid`, `local_address`,
                     `remote_address`, `send`, `recv`,
                     `do_not_reverse_lookup`, `__sendmsg*`, `__recvmsg*`.
  - `ipsocket.c`    — `IPSocket`: `addr`, `peeraddr`, `recvfrom`, `getaddress`.
  - `tcpsocket.c` / `tcpserver.c` — `TCPSocket#initialize`, `TCPServer#accept`.
  - `udpsocket.c` / `unixsocket.c` / `unixserver.c` — same shape.
  - `raddrinfo.c`   — `Addrinfo` (the big one): `initialize`, `getaddrinfo`,
                     `ip`/`tcp`/`udp`/`unix` factories, `afamily`, `pfamily`,
                     `socktype`, `protocol`, `canonname`, `ipv4?`/`ipv6?`/
                     `unix?`/`ip?`, `ip_address`, `ip_port`, `to_sockaddr`,
                     `inspect_sockaddr`, `getnameinfo`, `marshal_dump/load`,
                     plus the many `ipv6_*` predicates.
  - `option.c`      — `Socket::Option` (`.new`, `#family`, `#level`,
                     `#optname`, `#data`, `.int`, `.byte`, `#int`).
  - `ancdata.c`     — `Socket::AncillaryData` and `sendmsg`/`recvmsg` plumbing.
  - `ifaddr.c`      — `Socket::Ifaddr`.
  - `constants.c` / `mkconstants.rb` — all `Socket::AF_*` / `SOCK_*` /
                     `IPPROTO_*` / `MSG_*` / `SOL_*` / `SO_*` / `IP_*` /
                     `TCP_*` / `UDP_*` constants.
  - `init.c`        — `rsock_init_sock` (wraps an fd into a Ruby socket object).
  - `rubysocket.h`  — shared C headers (`rsock_family_arg`, `rsock_socket`, …).

> For a first pass, mirror **socket.rb** (Ruby) as exactly as possible, and
> provide native C bindings only for the OS-touching primitives. Most of the
> high-level sugar (`Socket.tcp`, `accept_loop`, `udp_server_loop`,
> `Addrinfo#connect`, etc.) is *pure Ruby* already in socket.rb and can be
> carried over almost verbatim.

### Spinel source tree
- Compiler: `src/*.c` (`analyze_scope.c`, `analyze_infer.c`, `codegen.c`,
  `codegen_call.c`, `codegen_call_recv.c`, `compiler.c`, `spinel_parse.c`).
- Runtime / stable ABI header: `lib/spinel/runtime.h` (pulls in `sp_gc.h` +
  `sp_alloc.h`). **A carried package's C must include only this header.**
- Existing POSIX primitives we reuse: `lib/sp_net.h` + `lib/sp_net.c`
  (`sp_net_listen`, `sp_net_accept`, `sp_net_connect`, `sp_net_close`,
  `sp_net_recv_*`, `sp_net_write_*`, `sp_net_poll_*`, fork/process helpers).
- File/IO primitives (reference for fd→object wrapping): `lib/sp_io.h`
  (`sp_io_fdopen`, `sp_File`, `sp_File_fileno`).
- Existing native packages (the templates to copy):
  - `packages/stringio/`  — `stringio.rb`, `sp_stringio.c`, `sp_stringio.h`,
    `spin.toml`, `sp_stringio.o`. **This is the closest model for our socket
    package** (a `native_struct` + `native_new` + `native_method` typed object).
  - `packages/json/`      — `json.rb`, `sp_json.c`, `sp_json.h`, `sp_json.o`,
    `spin.toml` (a `native_func` + `native_obj_reflect` module).
  - `packages/strscan/`, `packages/base64/` — also native packages.
- Makefile: `BUNDLED_NATIVE_OBJS` (around line 64) and per-package `.o` rules
  (around lines 250–280). We must add `packages/socket/sp_socket.o` here.
- `src/spinel_parse.c` — `sp_lib_is_native()` (line ~2189). We must add
  `"socket"` to this list so `require "socket"` is treated as a native
  (require-gated) feature.

---

## 2. The Spinel native-binding mechanism (Path B typed object)

This is the mechanism we use. Reference implementation: `packages/stringio`.

### 2.1 The declaration file (e.g. `packages/socket/socket.rb`)

A Ruby module in a package declares the C bindings. The compiler reads these
at analyze time and emits **direct typed C calls** — no FFI boxing:

```ruby
module SocketPackage            # name is arbitrary; native_lib gates the require
  native_lib "socket"           # require "socket" becomes the gating feature
  native_obj "packages/socket/sp_socket.o"   # carried C, linked only when required

  # One native_struct per C-backed class. The struct's first field must be
  # `mrb_int cls_id;` (the runtime stamps the assigned class id into it, so the
  # object flows through poly/array/cls_id dispatch like any object).
  native_struct "BasicSocket", "sp_BasicSocket", "sp_BasicSocket_free"
  native_struct "Socket",      "sp_Socket",      "sp_Socket_free"
  native_struct "IPSocket",    "sp_IPSocket",     "sp_IPSocket_free"
  native_struct "TCPSocket",   "sp_TCPSc",        "sp_TCPSc_free"
  # ... one per concrete class

  # Arity-keyed constructors. The C symbol takes the compiler-assigned cls_id
  # FIRST, then the args.
  native_new [:int, :int, :int],          "sp_Socket_new"        # Socket.new(family,type,proto)
  native_new [],                          "sp_TCPSocket_new"     # TCPSocket.new(...)
  # ...

  # Instance methods: native_method :name, [arg_specs], ret_spec, "csym"
  # receiver (the sp_* struct pointer) is passed FIRST by the codegen.
  native_method :close_read,  [],        :int,    "sp_BasicSocket_close_read"
  native_method :shutdown,    [:int],    :int,    "sp_BasicSocket_shutdown"
  native_method :getsockname, [],        :string, "sp_BasicSocket_getsockname"
  native_method :recv,        [:int],    :string, "sp_BasicSocket_recv"
  native_method :send,        [:string], :int,    "sp_BasicSocket_send"
  # ...
end
```

Spec strings (`native_method` / `native_new` args & ret):
- `:any`    → `sp_RbVal` (boxed poly)
- `:string` → `const char *`, returned as a Spinel String (NUL-terminated;
  stops at first embedded NUL)
- `:string?`→ nullable `const char *`, returned boxed nil when NULL
- `:int` / `:float` / `:bool` → `mrb_int` / `double` / `int`
- `:self`   → returns the receiver's struct pointer (chainable methods like
  `<<`); codegen types it as the class itself.
- `:nil` / `:void` → void.

See `native_spec_to_ty()` (`src/compiler.c:824`) and `native_c_type()`
(`src/codegen_util.c:798`) for the exact mapping. **There is no `:ptr` or
`:binstr` spec for native methods** — binary-safe recv must be done via the
returned-byte-count + `sp_str_from_bytes` inside the C function itself, or by
declaring the method `:string` and building a binary-safe String in C via
`sp_str_from_bytes(data, len)` (which preserves embedded NULs — see
`lib/sp_alloc.h:164`). Use `sp_str_from_bytes` for binary reads.

### 2.2 How the compiler wires it up (for context, not action)
- `analyze_scope.c` pre-scan: `native_lib "feat"` → require-gate feature name
  stamped on every `native_func`/`native_obj` of the module.
- `native_struct "Name", "sp_CStruct"` → registers `Name` as a **native class**
  (`is_native_class = 1`, gets a `cls_id` index `ci`; type is `TY_OBJECT + ci`).
- `codegen.c` ~4940–4990: emits forward `typedef struct sp_CStruct_s sp_CStruct;`,
  prototypes each `native_method`/`native_new` C symbol (ctor returns
  `sp_CStruct *` and takes `mrb_int` cls_id first; instance method takes
  `sp_CStruct *` receiver first), and emits the
  `/* SPINEL_LINK_OBJ: packages/socket/sp_socket.o */` marker (only when the
  `socket` feature is enabled, i.e. `require "socket"` appears).
- `src/main.c` scrapes `SPINEL_LINK_OBJ:` and adds the `.o` to the link line,
  resolved against the compiler's base dir (beside `lib/` and `packages/`).
- `codegen_call_recv.c` ~6691 and `codegen_call.c` ~2832: native instance
  method / no-arg dispatch arms. **Important limitation (see §4).**

### 2.3 The C side (e.g. `packages/socket/sp_socket.h` + `sp_socket.c`)
- Include **only** `spinel/runtime.h` (the stable ABI). That gives you
  `sp_RbVal`, `SP_TAG_*`, `sp_gc_alloc`, `sp_str_alloc`, `sp_str_from_bytes`,
  `sp_str_set_len`, `sp_box_*`, `sp_raise_cls`, `sp_oom_die`, container
  reflection hooks. (Copy the include pattern from `packages/stringio/`.)
- Each struct's first member is `mrb_int cls_id;`:
  ```c
  typedef struct sp_Socket_s { mrb_int cls_id; int fd; /* ... */ } sp_Socket;
  ```
  The constructor receives `cls_id` as its first arg and stores it:
  ```c
  sp_Socket *sp_Socket_new(mrb_int cls_id, mrb_int family, mrb_int type, mrb_int proto) {
    sp_Socket *s = (sp_Socket *)sp_gc_alloc(sizeof(sp_Socket), sp_Socket_free, NULL);
    memset(s, 0, sizeof *s); s->cls_id = cls_id;
    s->fd = sp_net_socket((int)family, (int)type, (int)proto);
    return s;
  }
  ```
- The finalizer closes the fd: `void sp_Socket_free(void *p){ sp_Socket *s=(sp_Socket*)p; if(s->fd>=0) sp_net_close(s->fd); }`.
- Returning strings: `return sp_str_from_bytes(data, len);` (binary-safe) or
  `return sp_str_alloc(...)` + `sp_str_set_len(...)`. Returning nil: return
  `NULL` from a `:string?` method.
- Raising: `sp_raise_cls("Errno::ECONNREFUSED", "...")` (class name is a string;
  `Errno::*` and other named exceptions are handled by the `SP_CLASS_BY_NAME`
  path — see §3).

---

## 3. Runtime / ABI essentials the C side may use

From `lib/spinel/runtime.h` (via `spinel/runtime.h`):

- **Boxed value**: `sp_RbVal` (`lib/sp_gc.h:48`) — tagged union
  (`tag`, `cls_id`, `v.{i,s,f,b,p}`). Tags: `SP_TAG_INT/STR/FLT/BOOL/NIL/OBJ/
  SYM/CLASS/...` (`src/.../sp_runtime.h:1722+`).
- **Boxing helpers** (`lib/sp_alloc.h`): `sp_box_int`, `sp_box_str`,
  `sp_box_nil`, `sp_box_bool`, `sp_box_float`, `sp_box_obj(p, cls_id)`,
  `sp_box_nullable_str`, `sp_box_poly_array`, etc.
- **String heap** (`lib/sp_alloc.h`): `sp_str_alloc(len)`, `sp_str_alloc_raw`,
  `sp_str_set_len(s, len)`, `sp_str_from_bytes(data, len)` (binary-safe),
  `sp_str_empty`.
- **GC alloc**: `void *sp_gc_alloc(size_t, void(*fin)(void*), void(*scn)(void*));`
  — `fin` is called on collect (free fd / free malloc), `scn` marks any GC
  pointers the struct holds (NULL if none).
- **Errors**: `sp_raise_cls(const char *cls, const char *msg)` (noreturn).
  `cls` is a class name string; `Errno::E*` and other name-only classes work.
- **Container/poly**: `sp_PolyArray`, `sp_box_*_array`, and the reflection
  hooks (only needed if we serialize socket objects — we don't).
- **Object header convention**: every carried struct starts with `mrb_int
  cls_id;`. Read it back with `sp_obj_cls_id_of(p)` (`lib/sp_runtime.h:1850`).

File/IO reference (`lib/sp_io.h`): `sp_File` wraps a `FILE*`. For sockets we do
**not** reuse `sp_File`; we define our own `sp_Socket` structs carrying a raw
`int fd`. (Ruby's sockets are fd-backed, not stdio-backed, so a raw fd is the
right model — matches `basicsocket.c`'s `fp->fd`.)

---

## 4. CRITICAL codegen limitation: native methods are NOT inherited

`comp_native_method_find` (`src/compiler.c:875`) and the codegen dispatch arms
(`codegen_call_recv.c:6691`, `codegen_call.c:2832`) look up a native method by
**the receiver's exact class id only** — they do **not** walk the superclass
chain. Consequence:

> If `TCPSocket` is a subclass of `IPSocket`/`BasicSocket`, declaring a native
> method only on `BasicSocket` will **not** make it callable on a `TCPSocket`
> instance. You must declare every native method on **every concrete class**
> that should respond to it (or implement shared behavior in the pure-Ruby
> `socket.rb` and have the leaf classes delegate).

Two viable strategies:
1. **Declare the full method set on each class** (repetition, but mechanical —
   the C symbol just takes the struct pointer and reads `fd` from the common
   first bytes; if the structs share a common prefix you can `typedef` them as
   the same struct or cast). Simplest and matches how `stringio` works.
2. **Keep the class hierarchy in Ruby** (`class TCPSocket < IPSocket <
   BasicSocket < Socket`) for *shared Ruby-level* logic (e.g. `connect_address`,
   `sendmsg`), and bind only the OS primitives on each concrete class. Note
   that the Ruby superclass (`< IO` in CRuby) has **no Spinel equivalent**
   (`IO` is a builtin `TY_IO`/`sp_File`, not a user class). So in socket.rb we
   should **drop the `< IO` / `< BasicSocket` superclass edges that point at
   non-native classes**, or declare `BasicSocket`/`Socket`/etc. all as native
   classes with explicit (duplicate) method bindings. The safest first pass:
   declare `BasicSocket`, `Socket`, `IPSocket`, `TCPSocket`, `TCPServer`,
   `UDPSocket`, `UNIXSocket`, `UNIXServer`, `Addrinfo`, `Socket::Option`,
   `Socket::Ifaddr` each as a `native_struct`, and bind the needed methods on
   each (you can give them all the *same* C struct — `sp_Socket` — if their
   fields are identical, and just declare separate `native_struct` names so the
   cls_id dispatch distinguishes them; or share a common header struct and cast).

`resolve_parents` (`analyze_scope.c:2577`) only links a parent when it resolves
to a registered Spinel class, so a dangling `< IO` would simply be ignored
(parent = -1). Confirmed: a native class with no resolved parent is fine.

---

## 5. What `sp_net` already provides (reuse these)

From `lib/sp_net.h` (TCP + process + poll primitives; all `sp_net_` prefixed):

- Lifecycle (TCP-oriented): `sp_net_listen(int port, int reuseport)`,
  `sp_net_accept(int sfd)`, `sp_net_accept_nb(int sfd)`,
  `sp_net_connect(const char *host, int port)`, `sp_net_close(int fd)`,
  `sp_net_set_nonblock(int fd)`, `sp_net_set_nodelay(int fd)`.
- I/O: `sp_net_recv_some(int fd, int maxlen)`,
  `sp_net_recv_all(int fd, int max_bytes)` (static NUL-terminated buffer;
  `sp_net_bin_len` holds exact byte count for binary),
  `sp_net_write_str(int fd, const char *s)`,
  `sp_net_write_bytes(int fd, const char *data, int n)`.
- Poll: `sp_net_poll_reset()`, `sp_net_poll_add(int fd, int mode_bits)`,
  `sp_net_poll_run(int timeout_ms)`, `sp_net_poll_ready(int slot)`.
- Process: `sp_net_fork()`, `sp_net_exit(int)`, `sp_net_getpid()`,
  `sp_net_wait_any()`.
- Shell: `sp_net_shell_capture(const char*, int)`.
- Graceful shutdown: `sp_net_install_term_handlers()`,
  `sp_net_shutdown_requested()`.

**Caveat:** `sp_net_connect` does DNS + TCP and returns an fd; `sp_net_listen`
binds `INADDR_ANY`. These cover the common `TCPSocket.new(host, port)` and
`TCPServer.new(port)` paths. For the general `Socket.new(family, type, proto)`
+ explicit `bind`/`connect`/`sendto`/`recvfrom`/`setsockopt`/`getsockopt`/
`getsockname`/`getpeername`/`socketpair`/UNIX paths, **`sp_net` is insufficient**
— see §6.

---

## 6. What must be ADDED to `sp_net` (or a new `sp_socket` unit)

To back the full `socket` module we need generic BSD-socket primitives that
`sp_net` currently lacks. Recommended: **extend `lib/sp_net.h` / `lib/sp_net.c`**
(these are not part of the per-require `.o`; they are linked into the runtime
archive and callable from the package C — but note `sp_net.c` is currently
compiled into `libspinel_rt.a`; check `Makefile`/build whether adding symbols
there is acceptable, or place new generic socket helpers in the package's own
`sp_socket.c`). The cleanest first-pass approach is to put **all** socket
primitives in `packages/socket/sp_socket.c` and have it call libc
`<sys/socket.h>` directly (it's a separate TU, already allowed to include
system headers — `stringio`'s `.c` includes `<stdlib.h>/<string.h>`; JSON's
includes the stable ABI only, but a socket package will need `<sys/socket.h>`,
`<netdb.h>`, `<arpa/inet.h>`, `<netinet/in.h>`, `<unistd.h>` etc.).

Needed new C functions (signatures are suggestions; tune to the `native_*`
spec system which only knows int/string/float/bool/any/self/nil):

```
int  sp_socket_open(int family, int type, int proto);   /* socket(2) */
int  sp_socket_bind(int fd, const void *sa, int len);   /* bind(2) */
int  sp_socket_connect(int fd, const void *sa, int len);/* connect(2) */
int  sp_socket_listen(int fd, int backlog);             /* listen(2) */
int  sp_socket_accept(int fd);                          /* accept(2) */
int  sp_socket_close(int fd);                           /* close(2) (or reuse sp_net_close) */
int  sp_socket_shutdown(int fd, int how);               /* shutdown(2) */
/* send/recv */
int  sp_socket_send(int fd, const char *buf, int len, int flags);
int  sp_socket_recv(int fd, char *buf, int len, int flags);   /* caller passes a buffer */
/* sendto/recvfrom — need a packed sockaddr buffer; see §7 */
int  sp_socket_sendto(int fd, const char *buf, int len, int flags, const void *sa, int salen);
/* getsockname/getpeername — fill a caller buffer (sockaddr storage) */
int  sp_socket_getsockname(int fd, void *sa, int *salen);
int  sp_socket_getpeername(int fd, void *sa, int *salen);
/* setsockopt/getsockopt — optval as a byte buffer */
int  sp_socket_setsockopt(int fd, int level, int optname, const void *val, int len);
int  sp_socket_getsockopt(int fd, int level, int optname, void *val, int *len);
/* socketpair */
int  sp_socket_socketpair(int family, int type, int proto, int fds[2]);
/* sockaddr packing from Ruby arrays / strings */
/* pack_sockaddr_in(host, port) -> binary sockaddr (string of bytes) */
const char *sp_socket_pack_in(const char *host, int port, int *outlen);
/* pack_sockaddr_un(path) -> binary sockaddr_un */
const char *sp_socket_pack_un(const char *path, int *outlen);
/* unpack_sockaddr_in(binary) -> [ip_string, port]  (two return values: use a
   small struct or two functions) */
const char *sp_socket_unpack_in_addr(const char *sa, int len);
int         sp_socket_unpack_in_port(const char *sa, int len);
/* getaddrinfo-based connect helper returning an fd (used by Socket.tcp) */
int  sp_socket_connect_byname(const char *host, int port);
```

Notes:
- `native_method` cannot return two values. For `unpack_sockaddr_in`/`addr`/
  `peeraddr` (which return `[addr, port, ...]` arrays), implement them as
  **pure Ruby** in `socket.rb` by parsing the binary sockaddr string returned
  by a `:string` native method, OR provide multiple native methods
  (`#ip_address`, `#ip_port`) like `Addrinfo` already does.
- Binary sockaddr strings: build with `sp_str_from_bytes(sa, len)` so embedded
  NULs survive (e.g. `sockaddr_un` paths, IPv6).
- `getsockopt`/`setsockopt` with `Socket::Option` objects: implement
  `Socket::Option#data` as a binary string (`sp_str_from_bytes`), and
  `Socket::Option.int` / `.byte` as small pure-Ruby or native helpers.

---

## 7. Constants (`Socket::AF_INET`, `SOCK_STREAM`, `IPPROTO_TCP`, `MSG_*`, …)

The `native_*` DSL binds methods and structs only — **not constants**. So
`Socket::AF_INET` etc. must be defined as ordinary Ruby constants. Two options:

1. **Pure-Ruby integer literals** in `socket.rb` (simplest, matches the values
   on the host — fine because Spinel targets the host platform):
   ```ruby
   class Socket
     AF_INET  = 2
     AF_INET6 = 10
     AF_UNIX  = 1
     SOCK_STREAM = 1
     SOCK_DGRAM  = 2
     IPPROTO_TCP = 6
     SOL_SOCKET  = 1
     SO_REUSEADDR = 2
     # ... see /home/user/ruby/ext/socket/constants.c + mkconstants.rb for the
     # full list (AF_*, PF_*, SOCK_*, IPPROTO_*, SOL_*, SO_*, IP_*, TCP_*,
     # UDP_*, MSG_*, SHUT_*, SOMAXCONN, etc.)
   end
   ```
2. Or `ffi_const` inside a module and re-assign — but plain literals are
   clearest. The authoritative list and exact integer values are generated by
   `mkconstants.rb` from the host; copy the relevant subset.

`Addrinfo`/`Socket` also use symbolic family/socktype args
(`Addrinfo.tcp("host", port)` → `:INET`/`:STREAM`). Provide `:INET` etc. as
constants or accept strings — mirror `rsock_family_arg`/`rsock_socktype_arg`
(`constants.c`) which map both string and integer forms.

---

## 8. Mapping the CRuby API to native bindings (first-pass checklist)

Carry `lib/socket.rb` over largely as-is (it is already pure Ruby). Add native
bindings (and possibly extend `sp_net`/add `sp_socket.c`) for the OS primitives
it ultimately calls. Key entry points:

### `Socket` (class, native_struct `sp_Socket`)
- `Socket.new(family, type, proto=nil)` → `sp_Socket_new(family, type, proto)`
  (or `Socket.new(fd)` form — optional).
- `Socket.tcp(host, port, …)` → pure Ruby in socket.rb (uses `Addrinfo` +
  `Socket.new` + `connect`); works once primitives exist.
- `Socket.tcp_server_sockets` / `tcp_server_loop` / `accept_loop` → pure Ruby.
- `Socket.udp_server_sockets` / `udp_server_loop` / `udp_server_recv` → pure Ruby.
- `Socket.unix(path)` / `unix_server_socket` / `unix_server_loop` → pure Ruby.
- `Socket.sockaddr_in(host, port)` → `sp_socket_pack_in` (or pure Ruby pack).
- `Socket.unpack_sockaddr_in(str)` → pure Ruby unpack of binary sockaddr.
- `Socket.getaddrinfo` / `getnameinfo` → wrap `getaddrinfo(3)`/`getnameinfo(3)`
  in a native func (return array of arrays).
- `Socket.gethostname` → `gethostname(2)` native func.
- `Socket.getservbyname` / `getservbyport` → `getservbyname(3)` native func.
- `Socket.ip_address_list` → iterate `getifaddrs(3)` (optional; can stub).

### `BasicSocket` (native_struct `sp_BasicSocket`; methods repeat on subclasses)
- `close_read`, `close_write`, `shutdown(how)`, `setsockopt`, `getsockopt`,
  `getsockname`, `getpeername`, `send`, `recv`, `local_address`,
  `remote_address` (last two return `Addrinfo` — build from the sockaddr
  string), `do_not_reverse_lookup` (ivar, pure Ruby).
- `send`/`recv` map to `sp_socket_send`/`sp_socket_recv` with a caller buffer;
  for convenience expose a `recv(len)` in Ruby that allocates a buffer, calls
  the native recv into it, and returns `sp_str_from_bytes`.

### `IPSocket` (native_struct `sp_IPSocket`)
- `addr`, `peeraddr`, `recvfrom`, `getaddress` → build from sockaddr strings.

### `TCPSocket` / `TCPServer` / `UDPSocket` / `UNIXSocket` / `UNIXServer`
- Constructors: `TCPSocket.new(host, port, …)` → resolve + `sp_socket_connect`
  (or reuse `sp_net_connect`). `UDPSocket.new(type=:INET)` → `sp_socket_open`.
  `UNIXSocket.new(path)` → `socket(AF_UNIX)+connect`. Servers bind+listen.
- `TCPServer#accept` / `UNIXServer#accept` → `sp_socket_accept` wrapped in a
  new `sp_TCPSocket`/`sp_UNIXSocket` object (set `cls_id`).
- Because of §4, declare the needed methods on **each** leaf class (or have the
  Ruby subclasses just call up to a shared helper that uses the underlying fd
  stored in the common struct prefix).

### `Addrinfo` (native_struct `sp_Addrinfo`)
- `initialize` (sockaddr array or binary sockaddr), `getaddrinfo`, `ip`/`tcp`/
  `udp`/`unix` factories, `afamily`, `pfamily`, `socktype`, `protocol`,
  `canonname`, `ipv4?`/`ipv6?`/`unix?`/`ip?`, `ip_address`, `ip_port`,
  `to_sockaddr`, `inspect_sockaddr`, `getnameinfo`, plus the `ipv6_*` predicates
  (most are simple bit/range checks on the packed sockaddr — pure Ruby is fine).
- `Addrinfo#connect` / `connect_from` / `bind` / `listen` → pure Ruby using
  `Socket.new` + `connect`/`bind`/`listen` (already in socket.rb).

### `Socket::Option` / `Socket::Ifaddr` / `Socket::AncillaryData`
- `Socket::Option.new(family, level, optname, data)` and `.int`/`.byte` —
  store fields in a native struct (or a plain Ruby Struct — simplest: model
  `Socket::Option` as a plain Ruby class holding ivars, no C needed, since it's
  just data; `BasicSocket#getsockopt` constructs one, `setsockopt` reads it).
- `Socket::AncillaryData` — same: plain Ruby class for first pass.
- `Socket::Ifaddr` — `Socket.ip_address_list` returns these; stub/optional.

---

## 9. Integration steps (concrete, for the implementation pass)

1. **Create `packages/socket/`** mirroring `packages/stringio/`:
   - `spin.toml` (copy `packages/stringio/spin.toml` verbatim — only the
     `[package] name = "socket"` differs).
   - `socket.rb` — the native-binding declarations (`native_lib "socket"`,
     `native_struct`/`native_new`/`native_method` blocks) **plus** a copy of
     CRuby's `lib/socket.rb` pure-Ruby logic (the `Addrinfo`, `Socket` class
     methods, `UDPSocket`/`TCPServer`/`UNIXServer` sugar). Keep the native
     class declarations in a separate `module SocketPackage` and the user
     classes (`class Socket < ...`, `class Addrinfo`, etc.) at top level, or
     interleave — the compiler only reads the `native_*` calls regardless of
     module nesting.
   - `sp_socket.h` + `sp_socket.c` — the C structs + native method bodies,
     including the generic BSD-socket primitives from §6 (include system
     headers directly).
   - `sp_socket.o` — built by Makefile.
2. **`Makefile`**: add `packages/socket/sp_socket.o` to `BUNDLED_NATIVE_OBJS`
   (line ~64) and add a build rule (copy the `packages/stringio/sp_stringio.o`
   rule, ~line 264, adjusting the `-Ipackages/socket` include and header deps).
3. **`src/spinel_parse.c`**: add `"socket"` to the `sp_lib_is_native()` array
   (~line 2191) so `require "socket"` is gated as a native feature (enables the
   `.o` link + matches `native_lib "socket"`).
4. **Declare all OS-touching methods on each concrete native class** (per §4).
5. **Define `Socket::*` / `AF_*` / `SOCK_*` / `IPPROTO_*` / … constants** as
   plain Ruby integer constants in `socket.rb` (source: `constants.c` /
   `mkconstants.rb`).
6. **Build & test**: `make`, then compile a small program that does
   `require "socket"; s = TCPSocket.new("localhost", 80); ...` (use
   `spin test` snapshot tests against CRuby as oracle, like
   `packages/stringio/test/`).

---

## 11. Implementation milestones (progress tracker)

Status legend: `[x]` done · `[ ]` pending. Last updated after Milestone 1.

### Milestone 1 — package scaffold + C primitives (DONE)
- [x] `packages/socket/spin.toml` — package manifest (copied from `stringio`).
- [x] `packages/socket/sp_socket.h` — `sp_Socket` (common cls_id+fd struct shared
      by all socket classes) and `sp_Addrinfo` structs, plus prototypes for every
      generic BSD-socket primitive and native method body.
- [x] `packages/socket/sp_socket.c` — implementation: `socket/bind/connect/
      listen/accept/shutdown/close/send/recv/sendto/recvfrom/getsockname/
      getpeername/setsockopt/getsockopt/socketpair`, sockaddr pack/unpack
      (IPv4/IPv6/UNIX, binary-safe), a `getaddrinfo` wrapper (returns a boxed poly
      array via `sp_box_poly_array` — note `sp_box_ptr_array` is NOT in the stable
      ABI), constructors, BasicSocket methods, Socket class methods, Addrinfo
      accessors. **Compiles cleanly** with
      `cc -c -O2 -Ilib -Ipackages/socket packages/socket/sp_socket.c -o /tmp/x.o`
      (exit 0).
- Notes / decisions:
  - All socket classes share one `sp_Socket` struct (common prefix cls_id+fd);
    separate `native_struct` names give distinct cls_id for dispatch (see §4).
  - Errors raised via `sp_raise_cls("Errno::E...", msg)`; `raise_syserr` maps
    common errno values to `Errno::*` names, message form `"<syscall>: <strerror>"`.
  - `getaddrinfo` returns `sp_box_poly_array(arr)` (a Spinel `Array`), not
    `sp_box_ptr_array`.

### Milestone 2 — `socket.rb` binding declarations (DONE)
- [x] `packages/socket/socket.rb`:
  - `module SocketPackage` with `native_lib "socket"`, `native_obj
    "packages/socket/sp_socket.o"`, one `native_struct` per class
    (`BasicSocket`, `Socket`, `IPSocket`, `TCPSocket`, `TCPServer`, `UDPSocket`,
    `UNIXSocket`, `UNIXServer`, `Addrinfo`), `native_new` per constructor, and a
    `native_method` per OS-touching instance/class method — **declared on EVERY
    concrete class** (no superclass inheritance of native methods, see §4).
  - Definitions of the user classes (`class Socket`, `class BasicSocket`,
    `class Addrinfo`, `class TCPSocket < ...`, etc.) — superclass edges that point
    at non-native classes (e.g. CRuby's `< IO`) are dropped (parent stays -1).
  - Carried-over pure-Ruby logic from CRuby `lib/socket.rb` (`Addrinfo` sugar,
    `Socket.tcp`/`tcp_server_loop`/`udp_server_loop`/`unix*`, `UDPSocket`,
    `TCPServer`, `UNIXServer`, `UDPSource`).
  - `Socket::*` / `AF_*` / `PF_*` / `SOCK_*` / `IPPROTO_*` / `SOL_*` / `SO_*` /
    `IP_*` / `TCP_*` / `UDP_*` / `MSG_*` / `SHUT_*` / `SOMAXCONN` constants as
    plain Ruby integer literals (source: `constants.c` / `mkconstants.rb`).
  - Design notes:
    - Native ops are exposed via double-underscore methods (`__connect`,
      `__bind`, `__listen`, `__accept_one`, `__send`, `__sendto`, `__raw_new`,
      `__from_fd`, `__getaddrinfo`, `__socketpair_fds`, `__pack_in`,
      `__pack_un`, `__in_addr`, `__in_port`, `__un_path`, `__from_bin_auto`, etc.)
      so the public Ruby API reads cleanly. `Addrinfo#connect`/`#bind`/`#listen`
      call the native `__connect`/`__bind`/`__listen` (which create/return a
      `Socket` / int), and `Addrinfo#connect` wraps the socket in a block form.
    - `Socket.getaddrinfo` returns a poly Array of binary sockaddr strings; the
      Ruby `Addrinfo.getaddrinfo` maps each through `Addrinfo.__from_bin_auto`
      (which reads `sa_family` to set the family).
    - `Socket.socketpair` returns a poly Array of two fds (ints); `UNIXSocket/
      TCPSocket.socketpair` wrap each fd.
    - `Socket#accept` / `TCPServer#accept` / `UNIXServer#accept` use
      `Socket.__accept_raw` (returns the raw fd) and wrap it in the right class
      (`TCPSocket.__from_fd` / `UNIXSocket.__from_fd`); `Socket#accept` returns the
      new `Socket` directly via `sp_Socket_accept_one`.
    - `Socket::Option` / `Socket::Ifaddr` / `Socket::AncillaryData` are plain Ruby
      data classes (no C) for the first pass.

### Milestone 3 — build wiring (PENDING)
- [ ] `Makefile`: add `packages/socket/sp_socket.o` to `BUNDLED_NATIVE_OBJS`
      (line ~64) and a build rule (copy the `packages/stringio/sp_stringio.o`
      rule ~line 264, with `-Ipackages/socket` and the socket headers).
- [ ] `src/spinel_parse.c`: add `"socket"` to the `sp_lib_is_native()` array
      (~line 2191) so `require "socket"` is gated as a native feature.

### Milestone 4 — build + link verification (PENDING)
- [ ] `make` the whole compiler; compile a small `require "socket"` program
      (e.g. `TCPSocket.new("localhost", 80)` round-trip, `Socket.tcp` server/client,
      `Addrinfo.getaddrinfo`, `UDPSocket`, `UNIXServer`) and confirm it links the
      `sp_socket.o` and runs.
- [ ] Add `packages/socket/test/*.rb` (+ `.expected`) snapshot tests against
      CRuby as the oracle (mirror `packages/stringio/test/`), run via `spin test`.

---

## 10. Quick reference: the `stringio` template we are copying

- `packages/stringio/stringio.rb` — see the header comment block; it shows the
  exact `native_lib` / `native_struct` / `native_new` / `native_method` shape
  and the `StringIO.open` pure-Ruby sugar pattern.
- `packages/stringio/sp_stringio.h` — struct with `mrb_int cls_id;` first
  field, constructor prototypes taking `mrb_int cls_id`, finalizer
  `sp_StringIO_free`, scan `sp_StringIO_scan_gc`.
- `packages/stringio/sp_stringio.c` — e.g. `sp_StringIO_new` does
  `sp_gc_alloc(sizeof(sp_StringIO), sp_StringIO_free, sp_StringIO_scan_gc)`,
  `s->cls_id = cls_id;`, returns the pointer; readers return
  `sp_str_alloc`/`sp_str_from_bytes` strings; raises via `sp_raise_cls`.
- `packages/stringio/spin.toml` — minimal `[package]` table.

This is the proven pattern; the socket package differs only in that it talks to
`<sys/socket.h>` instead of an in-memory buffer, and needs the wider method set
from §8.

---

## 12. Bring-up debug log (Milestone 1–4 completion notes)

All four milestones are now DONE and `require "socket"` works end-to-end
(TCP server/client, UDP datagram, `Addrinfo`, `Socket.getaddrinfo`,
`local_address`/`remote_address`, `recvfrom`/`recvmsg`). Key bugs found and
fixed during the implementation pass (the C/compiler fixes are also recorded
in `FIXES.md`):

1. **Native method arg-count must match the C signature exactly.** A
   `native_method :bind, [:string]` emits `sp_Socket_bind_raw(s, sa)` with only
   the declared args; any *extra* C parameters (e.g. an unused `int len`) are
   left uninitialized and produce garbage (`bind` → `EINVAL`). Fix: drop unused
   trailing params in `sp_Socket_bind_raw`/`connect_raw`/`sendto_raw` and derive
   the sockaddr length from the binary string via `sp_str_byte_len(sa)` inside C.
   Same for `sp_BasicSocket_recv`/`send`/`sendmsg`/`recvmsg`/`setsockopt`
   (drop the trailing `flags`/`len` params; they were being passed garbage from
   the call site, e.g. `recv` got `flags=44` = `MSG_TRUNC`-ish, which zeroed the
   returned buffer).

2. **`shutdown()` on an unconnected UDP socket raises `ENOTCONN`.** `BasicSocket#close`
   called `shutdown(SHUT_RDWR)` unconditionally; for a UDP socket this fails.
   Fix: `sp_socket_shutdown_quiet` ignores the error; `close` uses it.

3. **Binary sockaddr strings contain NUL bytes.** The `string` native type is
   treated as NUL-terminated (`sp_box_str`/boxing assumes NUL-terminated C
   strings). A 16-byte `sockaddr_in` packs as `[2,0,0,0,127,...]` — the 2nd byte
   is NUL, so a NUL-terminated view truncates it. We avoid this by ONLY ever
   passing the `sp_Str*` pointer (whose `sp_str_byte_len` reads the real length
   from the header) and never re-deriving length via `strlen` on these values.
   (`sp_Socket_pack_in_wrap` returns a `sp_str_alloc`+`sp_str_set_len` buffer;
   the boxed Ruby String keeps the correct length. Confirmed `recvfrom`/`bind`
   see `len==16`.) Keep sockaddr round-trips binary-safe.

4. **A Ruby method named the same as a native `csym` collides.** The compiler
   derives the generated C symbol for a Ruby method `recvfrom` on `BasicSocket`
   as `sp_BasicSocket_recvfrom` — which clashes with the native method's `csym`.
   Symptom: `conflicting types for 'sp_BasicSocket_recvfrom'` (the header
   prototype vs. the compiler's synthesized 2-arg forward-decl). Fix: rename the
   native `csym`s to `_raw` (`sp_BasicSocket_recvfrom_raw`/`recvmsg_raw`) and
   keep the Ruby wrappers named `recvfrom`/`recvmsg` (they call the `_raw`
   natives and wrap the binary sockaddr in a pure-Ruby `Addrinfo`). General rule:
   any native method that needs a Ruby wrapper must use a `csym` distinct from
   the `sp_<Class>_<method>` form the compiler would generate for that Ruby name.

5. **`Addrinfo` is a pure-Ruby class; C `sp_Addrinfo` objects lack `@sockaddr`.**
   `local_address`/`remote_address`/`recvfrom`/`recvmsg` return binary sockaddr
   strings from C, then wrap them in `Addrinfo.new(bin)` (single-arg form, which
   derives `@family` from `sa_family` via `SocketN.family_of`). The pure-Ruby
   `Addrinfo` accessors (`ip_address`/`ip_port`/`family`/`socktype`/`protocol`)
   read the `@sockaddr` ivar, so they work. Added `def family; @family; end`
   (the compiler returns `nil` for an undefined method instead of raising, so a
   missing accessor silently yields `nil`).

6. **Symbol→int coercion for `:STREAM`/`:DGRAM`/`:RAW`/`:TCP`/`:UDP`.** CRuby
   passes these as symbols to `getaddrinfo`/`socktype`. `Socket.getaddrinfo` and
   `Addrinfo.getaddrinfo` map them to their `SOCK_*`/`IPPROTO_*` ints (the native
   `getaddrinfo` wrapper expects ints; `:STREAM.to_i` is unsupported in Spinel).

7. **`UDPSocket#bind`/`#connect`/`#send(host,port)` sugar omitted.** The
   compiler routes a 1-arg `self.bind(sockaddr)` into a 2-arg Ruby method (filling
   the missing port with a default), re-resolving the host as a binary sockaddr
   and failing in `getaddrinfo`. Callers pass a sockaddr string to the inherited
   `BasicSocket#bind`/`#connect` (e.g. `udp.bind(Socket.sockaddr_in(port, host))`),
   matching CRuby's underlying API. `UDPSocket#send(mesg, flags, host, port)` is
   kept (it uses `Addrinfo.getaddrinfo` then `sendto`).

8. **Known runtime limitation (not a package bug):** `String == String` is not
   supported by the Spinel compiler (`unsupported equality`), so tests must avoid
   `str == str` and instead rely on snapshot `.expected` output. The project's
   `spin test` snapshot harness is the right way to assert behavior.

Compiler fixes in this pass (also in `FIXES.md`):
- `src/codegen_call.c`: `native_func` (Path B) `:int`/`:float`/`:bool` poly args
  now coerced via `sp_poly_to_i`/`sp_poly_to_f` (mirrors the `native_method`/
  `native_ctor` coercion added earlier).
