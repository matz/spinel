/* sp_socket.c -- the Spinel `socket` package (Path B typed native binding).
 *
 * Backs the Ruby `socket` module's OS-touching primitives with direct BSD
 * sockets calls. The struct + prototypes live in sp_socket.h. The header is
 * part of the stable package ABI (spinel/runtime.h) plus the system socket
 * headers this TU includes directly.
 *
 * Memory model: sp_Socket / sp_Addrinfo are GC-allocated (finalizer closes
 * the fd / frees canonname). Returned strings use the shared string heap via
 * sp_str_from_bytes (binary-safe) or sp_str_alloc + sp_str_set_len.
 *
 * Errors: this TU raises with sp_raise_cls("Errno::E...", msg) where the
 * message mimics CRuby's "<syscall>: <strerror>". */
#include "sp_socket.h"

#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static const char *sock_err(const char *syscall) {
  /* Build "<syscall>: <strerror(errno)>" on the shared string heap. */
  const char *es = strerror(errno);
  size_t need = strlen(syscall) + 2 + strlen(es) + 1;
  char *buf = (char *)sp_str_alloc(need);
  snprintf(buf, need, "%s: %s", syscall, es);
  sp_str_set_len(buf, strlen(buf));
  return buf;
}

static void raise_syserr(const char *syscall, int err) {
  if (err == 0) return;  /* success: nothing to raise (callers pass errno) */
  /* err is an errno value; raise Errno::<NAME>. */
  const char *name = "Errno::EUNKNOWN";
  switch (err) {
    case EACCES: name = "Errno::EACCES"; break;
    case EADDRINUSE: name = "Errno::EADDRINUSE"; break;
    case EADDRNOTAVAIL: name = "Errno::EADDRNOTAVAIL"; break;
    case EAFNOSUPPORT: name = "Errno::EAFNOSUPPORT"; break;
    case EAGAIN: name = "Errno::EAGAIN"; break;
    case EALREADY: name = "Errno::EALREADY"; break;
    case EBADF: name = "Errno::EBADF"; break;
    case ECONNREFUSED: name = "Errno::ECONNREFUSED"; break;
    case ECONNRESET: name = "Errno::ECONNRESET"; break;
    case EDESTADDRREQ: name = "Errno::EDESTADDRREQ"; break;
    case EHOSTUNREACH: name = "Errno::EHOSTUNREACH"; break;
    case EINPROGRESS: name = "Errno::EINPROGRESS"; break;
    case EINTR: name = "Errno::EINTR"; break;
    case EINVAL: name = "Errno::EINVAL"; break;
    case EISCONN: name = "Errno::EISCONN"; break;
    case ENETDOWN: name = "Errno::ENETDOWN"; break;
    case ENETUNREACH: name = "Errno::ENETUNREACH"; break;
    case ENOBUFS: name = "Errno::ENOBUFS"; break;
    case ENOPROTOOPT: name = "Errno::ENOPROTOOPT"; break;
    case ENOTCONN: name = "Errno::ENOTCONN"; break;
    case ENOTSOCK: name = "Errno::ENOTSOCK"; break;
    case EOPNOTSUPP: name = "Errno::EOPNOTSUPP"; break;
    case EPERM: name = "Errno::EPERM"; break;
    case EPROTO: name = "Errno::EPROTO"; break;
    case EPROTONOSUPPORT: name = "Errno::EPROTONOSUPPORT"; break;
    case EPROTOTYPE: name = "Errno::EPROTOTYPE"; break;
    case ETIMEDOUT: name = "Errno::ETIMEDOUT"; break;
    default: break;
  }
  sp_raise_cls(name, sock_err(""));
}

/* ------------------------------------------------------------------ */
/* generic BSD socket primitives                                      */
/* ------------------------------------------------------------------ */

int sp_socket_open(int family, int type, int proto) {
  int fd = socket(family, type, proto);
  if (fd < 0) raise_syserr("socket", errno);
  return fd;
}

int sp_socket_bind(int fd, const struct sockaddr *sa, int len) {
  if (bind(fd, sa, (socklen_t)len) < 0) { raise_syserr("bind", errno); }
  return 0;
}

int sp_socket_connect(int fd, const struct sockaddr *sa, int len) {
  if (connect(fd, sa, (socklen_t)len) < 0) {
    /* EINPROGRESS is a legitimate non-blocking "in progress" result; callers
       that loop on it catch it via errno themselves, so we still raise for
       the common blocking case here. */
    raise_syserr("connect", errno);
  }
  return 0;
}

int sp_socket_listen(int fd, int backlog) {
  if (listen(fd, backlog) < 0) { raise_syserr("listen", errno); }
  return 0;
}

int sp_socket_accept(int fd) {
  int c = accept(fd, NULL, NULL);
  if (c < 0) { raise_syserr("accept", errno); }
  return c;
}

int sp_socket_shutdown(int fd, int how) {
  if (shutdown(fd, how) < 0) { raise_syserr("shutdown", errno); }
  return 0;
}
/* Best-effort shutdown: ignore errors expected for connectionless sockets
   (e.g. shutdown() on an unconnected UDP socket returns ENOTCONN). Used by
   #close so a UDP socket can be closed without raising. */
void sp_socket_shutdown_quiet(int fd, int how) {
  if (fd >= 0) (void)shutdown(fd, how);
}

int sp_socket_close(int fd) {
  if (fd >= 0) { int r = close(fd); (void)r; }
  return 0;
}

int sp_socket_send(int fd, const char *buf, int len, int flags) {
  ssize_t n = send(fd, buf, (size_t)len, flags);
  return (int)n;
}

int sp_socket_recv(int fd, char *buf, int len, int flags) {
  ssize_t n = recv(fd, buf, (size_t)len, flags);
  if (n < 0) { raise_syserr("recv", errno); }
  return (int)n;  /* 0 == EOF, caller distinguishes */
}

int sp_socket_sendto(int fd, const char *buf, int len, int flags,
                     const struct sockaddr *sa, int salen) {
  ssize_t n = sendto(fd, buf, (size_t)len, flags, sa, (socklen_t)salen);
  if (n < 0) { raise_syserr("sendto", errno); }
  return (int)n;
}

int sp_socket_recvfrom(int fd, char *buf, int len, int flags,
                       struct sockaddr *sa, int *salen) {
  socklen_t sl = salen ? (socklen_t)*salen : 0;
  ssize_t n = recvfrom(fd, buf, (size_t)len, flags, sa, &sl);
  if (n < 0) { raise_syserr("recvfrom", errno); }
  if (salen) *salen = (int)sl;
  return (int)n;
}

int sp_socket_getsockname(int fd, struct sockaddr *sa, int *salen) {
  socklen_t sl = (socklen_t)*salen;
  if (getsockname(fd, sa, &sl) < 0) { raise_syserr("getsockname", errno); }
  *salen = (int)sl;
  return 0;
}

int sp_socket_getpeername(int fd, struct sockaddr *sa, int *salen) {
  socklen_t sl = (socklen_t)*salen;
  if (getpeername(fd, sa, &sl) < 0) { raise_syserr("getpeername", errno); }
  *salen = (int)sl;
  return 0;
}

int sp_socket_setsockopt(int fd, int level, int optname,
                         const char *val, int len) {
  if (setsockopt(fd, level, optname, val, (socklen_t)len) < 0)
    { raise_syserr("setsockopt", errno); }
  return 0;
}

int sp_socket_getsockopt(int fd, int level, int optname,
                         char *val, int *len) {
  socklen_t l = (socklen_t)*len;
  if (getsockopt(fd, level, optname, val, &l) < 0)
    { raise_syserr("getsockopt", errno); }
  *len = (int)l;
  return 0;
}

int sp_socket_socketpair(int family, int type, int proto, int fds[2]) {
  if (socketpair(family, type, proto, fds) < 0) { raise_syserr("socketpair", errno); }
  return 0;
}

/* ------------------------------------------------------------------ */
/* sockaddr packing / unpacking                                       */
/* ------------------------------------------------------------------ */

const char *sp_socket_pack_in(const char *host, int port, int *outlen) {
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = 0;
  hints.ai_family = AF_UNSPEC;
  char portbuf[16];
  snprintf(portbuf, sizeof portbuf, "%d", port);
  if (getaddrinfo(host, portbuf, &hints, &res) != 0 || !res) {
    sp_raise_cls("SocketError", sock_err("getaddrinfo"));
  }
  /* Prefer an IPv4 result if available, else the first. */
  struct addrinfo *ai = res;
  for (struct addrinfo *p = res; p; p = p->ai_next) {
    if (p->ai_family == AF_INET) { ai = p; break; }
  }
  int len = (int)ai->ai_addrlen;
  char *buf = (char *)sp_str_alloc((size_t)len);
  memcpy(buf, ai->ai_addr, (size_t)len);
  sp_str_set_len(buf, (size_t)len);
  *outlen = len;
  freeaddrinfo(res);
  return buf;
}

const char *sp_socket_pack_un(const char *path, int *outlen) {
  struct sockaddr_un sun;
  memset(&sun, 0, sizeof sun);
  sun.sun_family = AF_UNIX;
  size_t plen = strlen(path);
  if (plen >= sizeof(sun.sun_path)) plen = sizeof(sun.sun_path) - 1;
  memcpy(sun.sun_path, path, plen);
  sun.sun_path[plen] = '\0';
  /* On Linux the abstract namespace begins with a NUL byte; CRuby handles
     that via a leading '\0' in path. We copy verbatim, so an abstract name
     (path[0]=='\0') is preserved. */
  socklen_t slen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + plen);
  if (path[0] == '\0') slen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + plen + 1);
  char *buf = (char *)sp_str_alloc((size_t)slen);
  memcpy(buf, &sun, (size_t)slen);
  sp_str_set_len(buf, (size_t)slen);
  *outlen = (int)slen;
  return buf;
}

int sp_socket_unpack(const char *sa, int len, struct sockaddr_storage *out) {
  if (len < (int)sizeof(out->ss_family)) return -1;
  if ((size_t)len > sizeof(*out)) len = (int)sizeof(*out);
  memset(out, 0, sizeof(*out));
  memcpy(out, sa, (size_t)len);
  return 0;
}

const char *sp_socket_in_addr(const char *sa, int len) {
  struct sockaddr_storage ss;
  if (sp_socket_unpack(sa, len, &ss) < 0) return sp_str_empty;
  char buf[INET6_ADDRSTRLEN];
  if (ss.ss_family == AF_INET) {
    struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
    if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf)) return sp_str_empty;
  } else if (ss.ss_family == AF_INET6) {
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
    if (!inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof buf)) return sp_str_empty;
  } else {
    return sp_str_empty;
  }
  char *r = (char *)sp_str_alloc(strlen(buf));
  strcpy(r, buf);
  sp_str_set_len(r, strlen(r));
  return r;
}

int sp_socket_in_port(const char *sa, int len) {
  struct sockaddr_storage ss;
  if (sp_socket_unpack(sa, len, &ss) < 0) return 0;
  if (ss.ss_family == AF_INET)  return ntohs(((struct sockaddr_in *)&ss)->sin_port);
  if (ss.ss_family == AF_INET6) return ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
  return 0;
}

const char *sp_socket_un_path(const char *sa, int len) {
  struct sockaddr_storage ss;
  if (sp_socket_unpack(sa, len, &ss) < 0) return sp_str_empty;
  if (ss.ss_family != AF_UNIX) return sp_str_empty;
  struct sockaddr_un *sun = (struct sockaddr_un *)&ss;
  /* abstract socket: path[0]=='\0' — return as-is (leading NUL) */
  char *r = (char *)sp_str_alloc((size_t)len);
  memcpy(r, sun->sun_path, (size_t)len);
  sp_str_set_len(r, (size_t)len);
  return r;
}

/* ------------------------------------------------------------------ */
/* getaddrinfo wrapper                                                */
/* ------------------------------------------------------------------ */

sp_RbVal sp_socket_getaddrinfo(const char *node, const char *service,
                               int family, int socktype, int protocol, int flags) {
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = family;
  hints.ai_socktype = socktype;
  hints.ai_protocol = protocol;
  hints.ai_flags = flags;
  int err = getaddrinfo(node, service, &hints, &res);
  if (err != 0) {
    sp_raise_cls("SocketError",
                 (const char *)(gai_strerror(err) ? gai_strerror(err) : "getaddrinfo"));
  }
  sp_PolyArray *arr = sp_PolyArray_new();
  SP_GC_ROOT(arr);
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    sp_Addrinfo *ra = sp_Addrinfo_from_bin(0, ai->ai_family,
        ai->ai_socktype, ai->ai_protocol,
        (const char *)ai->ai_addr, (int)ai->ai_addrlen, NULL);
    sp_PolyArray_push(arr, sp_box_obj(ra, 0));  /* cls_id filled by caller */
  }
  freeaddrinfo(res);
  return sp_box_poly_array(arr);
}

/* ------------------------------------------------------------------ */
/* constructors                                                       */
/* ------------------------------------------------------------------ */

sp_Socket *sp_Socket_new(mrb_int cls_id, int family, int type, int proto) {
  int fd = sp_socket_open(family, type, proto);
  sp_Socket *s = (sp_Socket *)sp_gc_alloc(sizeof(sp_Socket),
                                          sp_BasicSocket_free, NULL);
  memset(s, 0, sizeof *s);
  s->cls_id = cls_id;
  s->fd = fd;
  s->family = family;
  s->type = type;
  s->proto = proto;
  s->do_not_reverse_lookup = 0;
  return s;
}

sp_Socket *sp_Socket_from_fd(mrb_int cls_id, int fd, int family, int type, int proto) {
  sp_Socket *s = (sp_Socket *)sp_gc_alloc(sizeof(sp_Socket),
                                          sp_BasicSocket_free, NULL);
  memset(s, 0, sizeof *s);
  s->cls_id = cls_id;
  s->fd = fd;
  s->family = family;
  s->type = type;
  s->proto = proto;
  s->do_not_reverse_lookup = 0;
  return s;
}

sp_Addrinfo *sp_Addrinfo_new(mrb_int cls_id, int family, int socktype,
                             int protocol, const struct sockaddr *sa, int len,
                             const char *canonname) {
  sp_Addrinfo *a = (sp_Addrinfo *)sp_gc_alloc(sizeof(sp_Addrinfo),
                                              sp_Addrinfo_free, NULL);
  memset(a, 0, sizeof *a);
  a->cls_id = cls_id;
  a->family = family;
  a->socktype = socktype;
  a->protocol = protocol;
  if (len > 0 && (size_t)len <= sizeof(a->sockaddr)) {
    memcpy(&a->sockaddr, sa, (size_t)len);
    a->socklen = len;
  }
  if (canonname) {
    a->canonname_len = (int)strlen(canonname);
    a->canonname = (char *)malloc((size_t)a->canonname_len + 1);
    if (a->canonname) { strcpy(a->canonname, canonname); }
  }
  return a;
}

sp_Addrinfo *sp_Addrinfo_from_bin(mrb_int cls_id, int family, int socktype,
                                  int protocol, const char *bin, int len,
                                  const char *canonname) {
  struct sockaddr_storage ss;
  if (sp_socket_unpack(bin, len, &ss) < 0) {
    sp_raise_cls("SocketError", "invalid sockaddr");
  }
  /* when family is unspecified/0, derive it from the sockaddr's sa_family so
     getaddrinfo results and raw-binary Addrinfo keep a usable family. */
  if (family <= 0) family = (int)ss.ss_family;
  if (socktype <= 0 && (family == AF_UNIX)) socktype = SOCK_STREAM;
  return sp_Addrinfo_new(cls_id, family, socktype, protocol,
                         (struct sockaddr *)&ss, len, canonname);
}

/* Enable SO_REUSEADDR so a restarted server can rebind immediately. */
static void sp_socket_setopt_reuseaddr(int fd) {
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
}

/* High-level constructors (used as native_new shapes for the socket classes).
   Each opens the fd (and, for TCP/server, connects/binds/listens). On failure
   they raise the matching Errno::* / SocketError class name. */

/* Extract a sp_PolyArray* from a boxed sp_RbVal using only the stable ABI
   (sp_poly_to_poly_array is NOT part of the frozen package surface). */
static sp_PolyArray *sp_rb_to_poly_array(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_POLY_ARRAY)
    return (sp_PolyArray *)v.v.p;
  return NULL;
}
/* Boxed-string element -> its char* (NULL if not a string). */
static const char *sp_rb_to_cstr(sp_RbVal v) {
  if (v.tag == SP_TAG_STR) return v.v.s;
  return NULL;
}

/* Format a port as a decimal service string (getaddrinfo service arg). */
static const char *sp_port_str(int port, char *buf, size_t n) {
  snprintf(buf, n, "%d", (int)port);
  return buf;
}
sp_Socket *sp_TCPSocket_new(mrb_int cls_id, const char *host, int port) {
  /* binary sockaddr strings (see sp_TCPServer_new for why not the Addrinfo form) */
    char pbuf[16];
  sp_RbVal res = sp_Socket_getaddrinfo_strings(host, sp_port_str(port, pbuf, sizeof pbuf), AF_UNSPEC, SOCK_STREAM, 0, 0);
  sp_PolyArray *arr = sp_rb_to_poly_array(res);
  SP_GC_ROOT(arr);
  if (!arr || arr->len == 0) sp_raise_cls("SocketError", "no address for connect");
  for (mrb_int i = 0; i < arr->len; i++) {
    sp_RbVal el = sp_PolyArray_get(arr, i);
    const char *bin = sp_rb_to_cstr(el);
    int len = bin ? (int)sp_str_byte_len(bin) : 0;
    struct sockaddr_storage ss;
    if (!bin || sp_socket_unpack(bin, len, &ss) < 0) continue;
    int fd = sp_socket_open((int)ss.ss_family, SOCK_STREAM, 0);
    if (fd < 0) continue;
    if (sp_socket_connect(fd, (struct sockaddr *)&ss, len) == 0) {
      sp_Socket *s = sp_Socket_from_fd(cls_id, fd, (int)ss.ss_family, SOCK_STREAM, 0);
      return s;
    }
    sp_socket_close(fd);
  }
  sp_raise_cls("Errno::ECONNREFUSED", "failed to connect");
  return NULL;
}

sp_Socket *sp_TCPServer_new(mrb_int cls_id, const char *host, int port) {
  /* Use the string-returning getaddrinfo: each element is a binary sockaddr
     string (sp_box_str) we can unpack into a sockaddr_storage for bind. The
     Addrinfo-returning helper (sp_socket_getaddrinfo) is for the Ruby-level
     Addrinfo API, not for low-level bind/connect. */
    char pbuf[16];
  sp_RbVal res = sp_Socket_getaddrinfo_strings(host, sp_port_str(port, pbuf, sizeof pbuf), AF_UNSPEC, SOCK_STREAM, 0, AI_PASSIVE);
  sp_PolyArray *arr = sp_rb_to_poly_array(res);
  SP_GC_ROOT(arr);
  if (!arr || arr->len == 0) sp_raise_cls("SocketError", "no address for bind");
  for (mrb_int i = 0; i < arr->len; i++) {
    sp_RbVal el = sp_PolyArray_get(arr, i);
    const char *bin = sp_rb_to_cstr(el);
    int len = bin ? (int)sp_str_byte_len(bin) : 0;
    struct sockaddr_storage ss;
    if (!bin || sp_socket_unpack(bin, len, &ss) < 0) continue;
    int fd = sp_socket_open((int)ss.ss_family, SOCK_STREAM, 0);
    if (fd < 0) continue;
    sp_socket_setopt_reuseaddr(fd);
    if (sp_socket_bind(fd, (struct sockaddr *)&ss, len) == 0 &&
        sp_socket_listen(fd, SOMAXCONN) == 0) {
      sp_Socket *s = sp_Socket_from_fd(cls_id, fd, (int)ss.ss_family, SOCK_STREAM, 0);
      return s;
    }
    sp_socket_close(fd);
  }
  sp_raise_cls("Errno::EADDRINUSE", "failed to bind");
  return NULL;
}

sp_Socket *sp_TCPServer_new_port(mrb_int cls_id, int port) {
  /* bind to 0.0.0.0 (IPv4) by default, like CRuby's TCPServer.new(nil, port). */
  struct sockaddr_in sin;
  memset(&sin, 0, sizeof sin);
  sin.sin_family = AF_INET;
  sin.sin_port = htons((uint16_t)port);
  sin.sin_addr.s_addr = htonl(INADDR_ANY);
  int fd = sp_socket_open(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) sp_raise_cls("Errno::EAFNOSUPPORT", "socket(2)");
  sp_socket_setopt_reuseaddr(fd);
  if (sp_socket_bind(fd, (struct sockaddr *)&sin, sizeof sin) < 0 ||
      sp_socket_listen(fd, SOMAXCONN) < 0) {
    sp_socket_close(fd);
    sp_raise_cls("Errno::EADDRINUSE", "failed to bind");
  }
  return sp_Socket_from_fd(cls_id, fd, AF_INET, SOCK_STREAM, 0);
}

sp_Socket *sp_UDPSocket_new(mrb_int cls_id, int family) {
  int fd = sp_socket_open(family <= 0 ? AF_INET : family, SOCK_DGRAM, 0);
  if (fd < 0) sp_raise_cls("Errno::EAFNOSUPPORT", "socket(2)");
  return sp_Socket_from_fd(cls_id, fd, family <= 0 ? AF_INET : family, SOCK_DGRAM, 0);
}

sp_Socket *sp_UNIXSocket_new(mrb_int cls_id, const char *path) {
  int len;
  const char *bin = sp_socket_pack_un(path, &len);
  struct sockaddr_storage ss;
  if (sp_socket_unpack(bin, len, &ss) < 0) sp_raise_cls("SocketError", "invalid unix path");
  int fd = sp_socket_open(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) sp_raise_cls("Errno::EAFNOSUPPORT", "socket(2)");
  if (sp_socket_connect(fd, (struct sockaddr *)&ss, len) < 0) {
    sp_socket_close(fd);
    sp_raise_cls("Errno::ECONNREFUSED", "failed to connect (unix)");
  }
  return sp_Socket_from_fd(cls_id, fd, AF_UNIX, SOCK_STREAM, 0);
}

sp_Socket *sp_UNIXServer_new(mrb_int cls_id, const char *path) {
  int len;
  const char *bin = sp_socket_pack_un(path, &len);
  struct sockaddr_storage ss;
  if (sp_socket_unpack(bin, len, &ss) < 0) sp_raise_cls("SocketError", "invalid unix path");
  int fd = sp_socket_open(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) sp_raise_cls("Errno::EAFNOSUPPORT", "socket(2)");
  if (sp_socket_bind(fd, (struct sockaddr *)&ss, len) < 0 ||
      sp_socket_listen(fd, SOMAXCONN) < 0) {
    sp_socket_close(fd);
    sp_raise_cls("Errno::EADDRINUSE", "failed to bind (unix)");
  }
  return sp_Socket_from_fd(cls_id, fd, AF_UNIX, SOCK_STREAM, 0);
}

/* ---- wrapper symbols called by the Ruby-side binding (socket.rb) ----
   These adapt the raw primitives to the exact arities/names the Ruby layer
   expects. Some take the sp_Socket* receiver (instance ops) and some are
   class-level helpers (prefixed sp_Socket__ / sp_Addrinfo__). */

/* Socket.new(fd) wrapper: returns a Socket wrapping an existing fd. */
sp_Socket *sp_Socket_from_fd_wrapper(mrb_int cls_id, int fd) {
  return sp_Socket_from_fd(cls_id, fd, AF_UNSPEC, 0, 0);
}

/* Socket#connect / #bind / #listen / #accept (instance ops). */
int sp_Socket_connect_raw(sp_Socket *s, const char *sa) {
  int len = sa ? (int)sp_str_byte_len(sa) : 0;
  struct sockaddr_storage ss;
  if (!sa || sp_socket_unpack(sa, len, &ss) < 0) sp_raise_cls("SocketError", "invalid sockaddr");
  return sp_socket_connect(s->fd, (struct sockaddr *)&ss, len);
}
int sp_Socket_bind_raw(sp_Socket *s, const char *sa) {
  int len = sa ? (int)sp_str_byte_len(sa) : 0;
  struct sockaddr_storage ss;
  if (!sa || sp_socket_unpack(sa, len, &ss) < 0) sp_raise_cls("SocketError", "invalid sockaddr");
  return sp_socket_bind(s->fd, (struct sockaddr *)&ss, len);
}
int sp_Socket_listen_raw(sp_Socket *s, int backlog) {
  return sp_socket_listen(s->fd, backlog);
}
/* accept: return only the new fd as an int (Ruby wraps it in a TCPSocket/etc).
   The native_func Socket.accept_raw takes the boxed socket and returns the fd. */
int sp_Socket_accept_one_raw(sp_RbVal sock_val) {
  sp_Socket *s = (sp_Socket *)sock_val.v.p;
  int c = sp_socket_accept(s->fd);
  return c;
}

/* Socket#accept (instance op): same, but takes the native sp_Socket* directly. */
int sp_Socket_accept_raw(sp_Socket *s) {
  return sp_socket_accept(s->fd);
}

/* Socket#send / #sendto (instance ops). */
int sp_Socket_send_raw(sp_Socket *s, const char *buf, int flags) {
  return sp_socket_send(s->fd, buf, (int)strlen(buf), flags);
}
int sp_Socket_sendto_raw(sp_Socket *s, const char *buf, int flags,
                          const char *sa) {
  int len = sa ? (int)sp_str_byte_len(sa) : 0;
  struct sockaddr_storage ss;
  if (!sa || sp_socket_unpack(sa, len, &ss) < 0) sp_raise_cls("SocketError", "invalid sockaddr");
  return sp_socket_sendto(s->fd, buf, (int)strlen(buf), flags,
                          (struct sockaddr *)&ss, len);
}

/* Addrinfo#connect / #bind / #listen over a freshly created Socket. These
   mirror CRuby's Addrinfo#connect_internal at the C boundary for speed; the
   pure-Ruby path in socket.rb is also available. */
sp_Socket *sp_Addrinfo_connect(sp_Addrinfo *a) {
  sp_Socket *s = sp_Socket_new(0, a->family, a->socktype, a->protocol);
  sp_socket_connect(s->fd, (struct sockaddr *)&a->sockaddr, a->socklen);
  return s;
}
sp_Socket *sp_Addrinfo_bind(sp_Addrinfo *a) {
  sp_Socket *s = sp_Socket_new(0, a->family, a->socktype, a->protocol);
  sp_socket_bind(s->fd, (struct sockaddr *)&a->sockaddr, a->socklen);
  return s;
}
int sp_Addrinfo_listen(sp_Addrinfo *a, int backlog) {
  sp_Socket *s = sp_Socket_new(0, a->family, a->socktype, a->protocol);
  sp_socket_bind(s->fd, (struct sockaddr *)&a->sockaddr, a->socklen);
  return sp_socket_listen(s->fd, backlog);
}

/* Wrappers referenced from the Ruby layer. */
sp_RbVal sp_Socket_getaddrinfo_wrap(const char *node, const char *service,
                                    int family, int socktype, int protocol, int flags) {
  return sp_socket_getaddrinfo(node, service, family, socktype, protocol, flags);
}
const char *sp_Socket_pack_in_wrap(const char *host, int port) {
  int len; return sp_socket_pack_in(host, port, &len);
}
const char *sp_Socket_pack_un_wrap(const char *path) {
  int len; return sp_socket_pack_un(path, &len);
}
const char *sp_Socket_in_addr_wrap(const char *sa) {
  return sp_socket_in_addr(sa, (int)sp_str_byte_len(sa));
}
int sp_Socket_in_port_wrap(const char *sa) {
  return sp_socket_in_port(sa, (int)sp_str_byte_len(sa));
}
const char *sp_Socket_un_path_wrap(const char *sa) {
  return sp_socket_un_path(sa, (int)sp_str_byte_len(sa));
}
int sp_Socket_family_wrap(const char *sa) {
  struct sockaddr_storage ss;
  int len = sa ? (int)sp_str_byte_len(sa) : 0;
  if (sp_socket_unpack(sa, len, &ss) < 0) return 0;
  return (int)ss.ss_family;
}

sp_Addrinfo *sp_Addrinfo_new_wrapper(mrb_int cls_id, int family, int socktype,
                                     int protocol, const char *bin) {
  return sp_Addrinfo_from_bin(cls_id, family, socktype, protocol, bin,
                              bin ? (int)strlen(bin) : 0, NULL);
}
sp_Addrinfo *sp_Addrinfo_new_canon_wrapper(mrb_int cls_id, int family, int socktype,
                                           int protocol, const char *bin,
                                           const char *canonname) {
  return sp_Addrinfo_from_bin(cls_id, family, socktype, protocol, bin,
                              bin ? (int)strlen(bin) : 0, canonname);
}

/* Build an Addrinfo from a binary sockaddr, auto-detecting the family from the
   sockaddr's sa_family (used by Addrinfo.new(binary_sockaddr)). */
sp_Addrinfo *sp_Addrinfo_from_bin_auto(mrb_int cls_id, const char *bin, int len) {
  struct sockaddr_storage ss;
  if (sp_socket_unpack(bin, len, &ss) < 0) sp_raise_cls("SocketError", "invalid sockaddr");
  int family = (int)ss.ss_family;
  /* best-effort socktype: derive from the family's common case */
  int socktype = (family == AF_UNIX) ? SOCK_STREAM : 0;
  int protocol = 0;
  return sp_Addrinfo_new(cls_id, family, socktype, protocol,
                         (struct sockaddr *)&ss, len, NULL);
}

/* native_new [string] shape for Addrinfo: a raw binary sockaddr. */
sp_Addrinfo *sp_Addrinfo_from_bin_auto_wrapper(mrb_int cls_id, const char *bin) {
  return sp_Addrinfo_from_bin_auto(cls_id, bin, bin ? (int)sp_str_byte_len(bin) : 0);
}

/* Build an Addrinfo (boxed) from the socket's local/peer name. */
static sp_RbVal sp_addrinfo_from_name(mrb_int cls_id, const char *bin, int len) {
  sp_Addrinfo *a = sp_Addrinfo_from_bin_auto(cls_id, bin, len);
  return sp_box_obj(a, cls_id);
}

/* BasicSocket#close: shut down both directions then close the fd. */
int sp_BasicSocket_close(sp_Socket *s) {
  if (s->fd >= 0) {
    sp_socket_shutdown_quiet(s->fd, SHUT_RDWR);
    sp_socket_close(s->fd);
    s->fd = -1;
  }
  return 0;
}

/* BasicSocket#recvfrom_raw: returns a boxed [mesg_str, binary_sockaddr] pair.
   The caller (Ruby recvfrom wrapper) builds a pure-Ruby Addrinfo from the
   binary sockaddr so accessors (which read @sockaddr) work. */
sp_RbVal sp_BasicSocket_recvfrom_raw(sp_Socket *s, int len) {
  char *buf = (char *)malloc((size_t)(len > 0 ? len : 1));
  if (!buf) sp_oom_die();
  struct sockaddr_storage from;
  int fromlen = (int)sizeof from;
  int n = sp_socket_recvfrom(s->fd, buf, len, 0, (struct sockaddr *)&from, &fromlen);
  if (n < 0) { free(buf); return sp_box_nil(); }
  char *mesg = (char *)sp_str_alloc((size_t)n);
  memcpy(mesg, buf, (size_t)n);
  sp_str_set_len(mesg, (size_t)n);
  free(buf);
  char *bin = (char *)sp_str_alloc((size_t)fromlen);
  memcpy(bin, &from, (size_t)fromlen);
  sp_str_set_len(bin, (size_t)fromlen);
  sp_PolyArray *arr = sp_PolyArray_new();
  SP_GC_ROOT(arr);
  sp_PolyArray_push(arr, sp_box_str(mesg));
  sp_PolyArray_push(arr, sp_box_str(bin));
  return sp_box_poly_array(arr);
}

/* BasicSocket#sendmsg: behaves like #send (single buffer). */
int sp_BasicSocket_sendmsg(sp_Socket *s, const char *buf) {
  int rr = sp_socket_send(s->fd, buf, (int)strlen(buf), 0);
  return rr;
}

/* BasicSocket#recvmsg: behaves like #recvfrom (returns [mesg, sockaddr]). */
sp_RbVal sp_BasicSocket_recvmsg_raw(sp_Socket *s, int len) {
  return sp_BasicSocket_recvfrom_raw(s, len);
}

/* BasicSocket#read_nonblock / write_nonblock: recv/send wrappers. */
const char *sp_BasicSocket_read_nonblock(sp_Socket *s, int len) {
  char *buf = (char *)malloc((size_t)(len > 0 ? len : 1));
  if (!buf) sp_oom_die();
  int n = sp_socket_recv(s->fd, buf, len, 0);
  if (n <= 0) { free(buf); return sp_str_empty; }
  char *mesg = (char *)sp_str_alloc((size_t)n);
  memcpy(mesg, buf, (size_t)n);
  sp_str_set_len(mesg, (size_t)n);
  free(buf);
  return mesg;
}
int sp_BasicSocket_write_nonblock(sp_Socket *s, const char *buf) {
  return sp_socket_send(s->fd, buf, (int)strlen(buf), 0);
}

/* BasicSocket#local_address / #remote_address: boxed Addrinfo. */
sp_RbVal sp_BasicSocket_local_address(sp_Socket *s) {
  char buf[128];
  int len = (int)sizeof buf;
  sp_socket_getsockname(s->fd, (struct sockaddr *)buf, &len);
  return sp_addrinfo_from_name(s->cls_id, buf, len);
}
sp_RbVal sp_BasicSocket_remote_address(sp_Socket *s) {
  char buf[128];
  int len = (int)sizeof buf;
  sp_socket_getpeername(s->fd, (struct sockaddr *)buf, &len);
  return sp_addrinfo_from_name(s->cls_id, buf, len);
}

/* getaddrinfo that returns an Array (poly) of binary sockaddr strings — the
   Ruby layer wraps each into an Addrinfo (it carries family/socktype/proto via
   the binary sockaddr's sa_family). Avoids boxing cls_id-dependent objects in C. */
sp_RbVal sp_Socket_getaddrinfo_strings(const char *node, const char *service,
                                        int family, int socktype, int protocol, int flags) {
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = family;
  hints.ai_socktype = socktype;
  hints.ai_protocol = protocol;
  hints.ai_flags = flags;
  int err = getaddrinfo(node, service, &hints, &res);
  if (err != 0) {
    sp_raise_cls("SocketError",
                 (const char *)(gai_strerror(err) ? gai_strerror(err) : "getaddrinfo"));
  }
  sp_PolyArray *arr = sp_PolyArray_new();
  SP_GC_ROOT(arr);
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    char *buf = (char *)sp_str_alloc((size_t)ai->ai_addrlen);
    memcpy(buf, ai->ai_addr, (size_t)ai->ai_addrlen);
    sp_str_set_len(buf, (size_t)ai->ai_addrlen);
    sp_RbVal el0 = sp_box_str(buf);
    sp_PolyArray_push(arr, el0);
  }
  freeaddrinfo(res);
  return sp_box_poly_array(arr);
}

/* socketpair: returns a poly Array of two fds (ints). Ruby wraps each. */
sp_RbVal sp_Socket_socketpair_fds(int family, int type, int proto) {
  int fds[2];
  sp_socket_socketpair(family, type, proto, fds);
  sp_PolyArray *arr = sp_PolyArray_new();
  SP_GC_ROOT(arr);
  sp_PolyArray_push(arr, sp_box_int(fds[0]));
  sp_PolyArray_push(arr, sp_box_int(fds[1]));
  return sp_box_poly_array(arr);
}

/* Socket#accept: returns a new Socket wrapping the accepted fd (self-cls_id). */
sp_Socket *sp_Socket_accept_one(mrb_int cls_id, sp_Socket *s) {
  int cfd = sp_socket_accept(s->fd);
  sp_Socket *c = sp_Socket_from_fd(cls_id, cfd, s->family, s->type, s->proto);
  return c;
}

/* ------------------------------------------------------------------ */
/* finalizers                                                         */
/* ------------------------------------------------------------------ */

void sp_BasicSocket_free(void *p) {
  sp_Socket *s = (sp_Socket *)p;
  if (s->fd >= 0) { close(s->fd); s->fd = -1; }
}

void sp_Addrinfo_free(void *p) {
  sp_Addrinfo *a = (sp_Addrinfo *)p;
  if (a->canonname) { free(a->canonname); a->canonname = NULL; }
}

/* ------------------------------------------------------------------ */
/* BasicSocket instance methods                                       */
/* ------------------------------------------------------------------ */

int sp_BasicSocket_close_read(sp_Socket *s) {
  if (s->fd < 0) sp_raise_cls("IOError", "closed stream");
  if (shutdown(s->fd, SHUT_RD) < 0) { raise_syserr("shutdown", errno); }
  return 0;
}

int sp_BasicSocket_close_write(sp_Socket *s) {
  if (s->fd < 0) sp_raise_cls("IOError", "closed stream");
  if (shutdown(s->fd, SHUT_WR) < 0) { raise_syserr("shutdown", errno); }
  return 0;
}

int sp_BasicSocket_shutdown(sp_Socket *s, int how) {
  return sp_socket_shutdown(s->fd, how);
}

int sp_BasicSocket_setsockopt(sp_Socket *s, int level, int optname,
                              const char *val) {
  int len = val ? (int)sp_str_byte_len(val) : 0;
  return sp_socket_setsockopt(s->fd, level, optname, val, len);
}

int sp_BasicSocket_getsockopt_len(sp_Socket *s, int level, int optname) {
  /* probe the option length with a large buffer */
  char tmp[1024];
  int len = (int)sizeof(tmp);
  sp_socket_getsockopt(s->fd, level, optname, tmp, &len);
  return len;
}

const char *sp_BasicSocket_getsockopt(sp_Socket *s, int level, int optname,
                                      int *outlen) {
  char *buf = (char *)sp_str_alloc(1024);
  int len = 1024;
  sp_socket_getsockopt(s->fd, level, optname, buf, &len);
  sp_str_set_len(buf, (size_t)len);
  *outlen = len;
  return buf;
}

const char *sp_BasicSocket_getsockname(sp_Socket *s) {
  struct sockaddr_storage ss;
  int len = (int)sizeof(ss);
  sp_socket_getsockname(s->fd, (struct sockaddr *)&ss, &len);
  char *buf = (char *)sp_str_alloc((size_t)len);
  memcpy(buf, &ss, (size_t)len);
  sp_str_set_len(buf, (size_t)len);
  return buf;
}

const char *sp_BasicSocket_getpeername(sp_Socket *s) {
  struct sockaddr_storage ss;
  int len = (int)sizeof(ss);
  sp_socket_getpeername(s->fd, (struct sockaddr *)&ss, &len);
  char *buf = (char *)sp_str_alloc((size_t)len);
  memcpy(buf, &ss, (size_t)len);
  sp_str_set_len(buf, (size_t)len);
  return buf;
}

/* recv: read up to len bytes, return a binary-safe string. A 0-byte read
   (EOF) returns the empty string; -1 is impossible (raises). */
const char *sp_BasicSocket_recv(sp_Socket *s, int len) {
  if (len < 0) len = 0;
  char *buf = (char *)sp_str_alloc((size_t)len > 0 ? (size_t)len : 1);
  ssize_t n = recv(s->fd, buf, (size_t)len, 0);
  if (n < 0) { raise_syserr("recv", errno); }
  sp_str_set_len(buf, (size_t)n);
  return buf;
}

int sp_BasicSocket_send(sp_Socket *s, const char *buf) {
  return sp_socket_send(s->fd, buf, (int)strlen(buf), 0);
}

int sp_BasicSocket_fileno(sp_Socket *s) { return s->fd; }
int sp_BasicSocket_closed_p(sp_Socket *s) { return s->fd < 0 ? 1 : 0; }
int sp_BasicSocket_getfd(sp_Socket *s) { return s->fd; }

/* ------------------------------------------------------------------ */
/* Socket class methods                                               */
/* ------------------------------------------------------------------ */

sp_Socket *sp_Socket_socketpair(mrb_int cls_id, int family, int type, int proto) {
  /* We return only the first fd as the Socket; the pair's second end is
     closed here for the simple one-value form. For full socketpair semantics
     callers should use the native func that exposes both fds. */
  int fds[2];
  sp_socket_socketpair(family, type, proto, fds);
  sp_Socket *a = sp_Socket_from_fd(cls_id, fds[0], family, type, proto);
  close(fds[1]);
  return a;
}

const char *sp_Socket_gethostname(void) {
  char buf[1024];
  if (gethostname(buf, sizeof buf) < 0) { raise_syserr("gethostname", errno); }
  buf[sizeof(buf) - 1] = '\0';
  char *r = (char *)sp_str_alloc(strlen(buf));
  strcpy(r, buf);
  sp_str_set_len(r, strlen(r));
  return r;
}

const char *sp_Socket_getservbyname(const char *name, const char *proto) {
  struct servent *se = getservbyname(name, proto && proto[0] ? proto : "tcp");
  if (!se) sp_raise_cls("SocketError", "no such service");
  char portbuf[16];
  snprintf(portbuf, sizeof portbuf, "%d", ntohs(se->s_port));
  char *r = (char *)sp_str_alloc(strlen(portbuf));
  strcpy(r, portbuf);
  sp_str_set_len(r, strlen(r));
  return r;
}

int sp_Socket_getservbyport(int port, const char *proto) {
  struct servent *se = getservbyport(htons((uint16_t)port),
                                     proto && proto[0] ? proto : "tcp");
  if (!se) sp_raise_cls("SocketError", "no such service");
  return ntohs(se->s_port);
}

/* ------------------------------------------------------------------ */
/* Addrinfo accessors                                                 */
/* ------------------------------------------------------------------ */

int sp_Addrinfo_afamily(sp_Addrinfo *a) { return a->family; }
int sp_Addrinfo_pfamily(sp_Addrinfo *a) { return a->family; }
int sp_Addrinfo_socktype(sp_Addrinfo *a) { return a->socktype; }
int sp_Addrinfo_protocol(sp_Addrinfo *a) { return a->protocol; }

const char *sp_Addrinfo_canonname(sp_Addrinfo *a) {
  if (!a->canonname) return sp_str_empty;
  char *r = (char *)sp_str_alloc((size_t)a->canonname_len);
  memcpy(r, a->canonname, (size_t)a->canonname_len);
  sp_str_set_len(r, (size_t)a->canonname_len);
  return r;
}

const char *sp_Addrinfo_to_sockaddr(sp_Addrinfo *a) {
  char *buf = (char *)sp_str_alloc((size_t)a->socklen > 0 ? (size_t)a->socklen : 1);
  memcpy(buf, &a->sockaddr, (size_t)a->socklen);
  sp_str_set_len(buf, (size_t)a->socklen);
  return buf;
}

int sp_Addrinfo_ipv4_p(sp_Addrinfo *a) { return a->family == AF_INET; }
int sp_Addrinfo_ipv6_p(sp_Addrinfo *a) { return a->family == AF_INET6; }
int sp_Addrinfo_unix_p(sp_Addrinfo *a) { return a->family == AF_UNIX; }
int sp_Addrinfo_ip_p(sp_Addrinfo *a) {
  return a->family == AF_INET || a->family == AF_INET6;
}

const char *sp_Addrinfo_ip_address(sp_Addrinfo *a) {
  return sp_socket_in_addr((const char *)&a->sockaddr, a->socklen);
}
int sp_Addrinfo_ip_port(sp_Addrinfo *a) {
  return sp_socket_in_port((const char *)&a->sockaddr, a->socklen);
}
const char *sp_Addrinfo_unix_path(sp_Addrinfo *a) {
  return sp_socket_un_path((const char *)&a->sockaddr, a->socklen);
}
