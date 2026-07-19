#ifndef SP_SOCKET_H
#define SP_SOCKET_H
/* socket -- a carried-C spin package (Path B typed object).

   Every socket class (BasicSocket/Socket/IPSocket/TCPSocket/TCPServer/
   UDPSocket/UNIXSocket/UNIXServer) shares the same underlying struct; the
   runtime stamps a distinct cls_id into each so poly/array/cls_id dispatch
   keeps them distinct, but the native method bodies only read the common
   prefix (cls_id + fd) and so can be shared across classes. The struct's
   first field is mrb_int cls_id (the object-header convention shared with
   user classes). Instances are GC-allocated; the finalizer closes the fd.

   This unit includes the stable package ABI (spinel/runtime.h) plus the
   system socket headers it needs directly (it is a separate translation
   unit and is not bound to the compiler-internal headers). */
#include "spinel/runtime.h"   /* sp_RbVal, mrb_int, sp_gc_alloc, sp_str_* */

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

/* A socket handle: an fd plus the address family it was created with
   (used to validate operations and to build Addrinfo objects). */
typedef struct sp_Socket_s {
  mrb_int cls_id;      /* object header: runtime class id, compiler-stamped */
  int fd;              /* the socket file descriptor, or -1 when closed */
  int family;          /* AF_INET / AF_INET6 / AF_UNIX, or -1 if unknown */
  int type;            /* SOCK_STREAM / SOCK_DGRAM, or -1 */
  int proto;           /* IPPROTO_*, or 0 */
  int do_not_reverse_lookup; /* mirrors BasicSocket.do_not_reverse_lookup */
} sp_Socket;

/* An Addrinfo: a resolved sockaddr plus its family/socktype/protocol and an
   optional canonname. The sockaddr is stored as a binary-safe byte buffer. */
typedef struct sp_Addrinfo_s {
  mrb_int cls_id;      /* object header */
  int family;          /* sa_family */
  int socktype;        /* SOCK_STREAM / SOCK_DGRAM / 0 */
  int protocol;        /* IPPROTO_* / 0 */
  int canonname_len;   /* bytes in canonname, or 0 */
  char *canonname;     /* malloc'd canonical name, or NULL */
  int socklen;         /* bytes in sockaddr */
  struct sockaddr_storage sockaddr; /* the packed sockaddr */
} sp_Addrinfo;

/* ---- generic BSD socket primitives (back the Socket/BasicSocket API) ---- */

/* socket(2): returns a new fd, or -1 on error (errno set). */
int sp_socket_open(int family, int type, int proto);
/* bind(2) / connect(2) / listen(2) / accept(2) / shutdown(2) / close(2). */
int sp_socket_bind(int fd, const struct sockaddr *sa, int len);
int sp_socket_connect(int fd, const struct sockaddr *sa, int len);
int sp_socket_listen(int fd, int backlog);
int sp_socket_accept(int fd);
int sp_socket_shutdown(int fd, int how);
int sp_socket_close(int fd);
/* send(2)/recv(2) into a caller buffer. recv returns byte count or -1. */
int sp_socket_send(int fd, const char *buf, int len, int flags);
int sp_socket_recv(int fd, char *buf, int len, int flags);
/* sendto(2)/recvfrom(2). from/outlen may be NULL to ignore the peer. */
int sp_socket_sendto(int fd, const char *buf, int len, int flags,
                     const struct sockaddr *sa, int salen);
int sp_socket_recvfrom(int fd, char *buf, int len, int flags,
                       struct sockaddr *sa, int *salen);
/* getsockname(2)/getpeername(2): fill *salen (in/out). */
int sp_socket_getsockname(int fd, struct sockaddr *sa, int *salen);
int sp_socket_getpeername(int fd, struct sockaddr *sa, int *salen);
/* setsockopt(2)/getsockopt(2): optval as a byte buffer of length len. */
int sp_socket_setsockopt(int fd, int level, int optname,
                         const char *val, int len);
int sp_socket_getsockopt(int fd, int level, int optname,
                         char *val, int *len);
/* socketpair(2): fills fds[2]; returns 0 or -1. */
int sp_socket_socketpair(int family, int type, int proto, int fds[2]);

/* ---- sockaddr packing / unpacking (binary-safe) ---- */
/* pack "host", port into a sockaddr_in/in6; returns a binary sockaddr string
   and writes its length to *outlen. NULL on resolution failure. */
const char *sp_socket_pack_in(const char *host, int port, int *outlen);
/* pack a unix path into a sockaddr_un; returns a binary sockaddr string. */
const char *sp_socket_pack_un(const char *path, int *outlen);
/* unpack a binary sockaddr into the supplied storage; returns 0/-1. */
int sp_socket_unpack(const char *sa, int len, struct sockaddr_storage *out);
/* helpers for Addrinfo#ip_address / #ip_port / #unix_path. */
const char *sp_socket_in_addr(const char *sa, int len);  /* "1.2.3.4" or "::1" */
int         sp_socket_in_port(const char *sa, int len);  /* host-order port */
const char *sp_socket_un_path(const char *sa, int len);  /* unix path */

/* getaddrinfo(3) used by Addrinfo.getaddrinfo / Socket.getaddrinfo. Returns a
   sp_Addrinfo array (GC-managed list) boxed as a sp_RbVal, or raises on
   failure. argc mirrors Ruby's Addrinfo.getaddrinfo(node, service, family,
   socktype, protocol, flags). */
sp_RbVal sp_socket_getaddrinfo(const char *node, const char *service,
                               int family, int socktype, int protocol, int flags);

/* ---- constructors (cls_id first, per the native_new convention) ---- */
sp_Socket  *sp_Socket_new(mrb_int cls_id, int family, int type, int proto);
sp_Socket  *sp_Socket_from_fd(mrb_int cls_id, int fd, int family, int type, int proto);
sp_Socket  *sp_TCPSocket_new(mrb_int cls_id, const char *host, int port);
sp_Socket  *sp_TCPServer_new(mrb_int cls_id, const char *host, int port);
sp_Socket  *sp_TCPServer_new_port(mrb_int cls_id, int port);
sp_Socket  *sp_UDPSocket_new(mrb_int cls_id, int family);
sp_Socket  *sp_UNIXSocket_new(mrb_int cls_id, const char *path);
sp_Socket  *sp_UNIXServer_new(mrb_int cls_id, const char *path);
sp_Addrinfo *sp_Addrinfo_new(mrb_int cls_id, int family, int socktype,
                             int protocol, const struct sockaddr *sa, int len,
                             const char *canonname);
sp_Addrinfo *sp_Addrinfo_from_bin(mrb_int cls_id, int family, int socktype,
                                  int protocol, const char *bin, int len,
                                  const char *canonname);
sp_Addrinfo *sp_Addrinfo_from_bin_auto(mrb_int cls_id, const char *bin, int len);
sp_Addrinfo *sp_Addrinfo_from_bin_auto_wrapper(mrb_int cls_id, const char *bin);
int  sp_BasicSocket_close(sp_Socket *s);
sp_RbVal sp_BasicSocket_recvfrom_raw(sp_Socket *s, int len);
int  sp_BasicSocket_sendmsg(sp_Socket *s, const char *buf);
sp_RbVal sp_BasicSocket_recvmsg_raw(sp_Socket *s, int len);
const char *sp_BasicSocket_read_nonblock(sp_Socket *s, int len);
int  sp_BasicSocket_write_nonblock(sp_Socket *s, const char *buf);
sp_RbVal sp_BasicSocket_local_address(sp_Socket *s);
sp_RbVal sp_BasicSocket_remote_address(sp_Socket *s);

/* ---- BasicSocket instance methods ---- */
int  sp_BasicSocket_close_read(sp_Socket *s);
int  sp_BasicSocket_close_write(sp_Socket *s);
int  sp_BasicSocket_shutdown(sp_Socket *s, int how);
int  sp_BasicSocket_setsockopt_native(sp_RbVal self, int level, int optname,
                                     const char *val);
int  sp_BasicSocket_getsockopt_len(sp_Socket *s, int level, int optname);
const char *sp_BasicSocket_getsockopt(sp_Socket *s, int level, int optname,
                                      int *outlen);
const char *sp_BasicSocket_getsockopt_bin(sp_RbVal self, int level, int optname);
const char *sp_BasicSocket_getsockname(sp_Socket *s);   /* binary sockaddr */
const char *sp_BasicSocket_getpeername(sp_Socket *s);   /* binary sockaddr */
const char *sp_BasicSocket_recv(sp_Socket *s, int len); /* binary-safe, flags=0 */
int  sp_BasicSocket_send(sp_Socket *s, const char *buf); /* len=strlen, flags=0 */
int  sp_BasicSocket_fileno(sp_Socket *s);
int  sp_BasicSocket_closed_p(sp_Socket *s);
int  sp_BasicSocket_getfd(sp_Socket *s);
void sp_BasicSocket_free(void *p);   /* GC finalizer: closes fd */
void sp_Addrinfo_free(void *p);      /* GC finalizer: frees canonname */

/* ---- Socket class methods (constructors / statics) ---- */
sp_Socket *sp_Socket_socketpair(mrb_int cls_id, int family, int type, int proto);
const char *sp_Socket_gethostname(void);
const char *sp_Socket_getservbyname(const char *name, const char *proto);
int         sp_Socket_getservbyport(int port, const char *proto);

/* ---- Socket instance op wrappers (called from socket.rb) ---- */
sp_Socket *sp_Socket_from_fd_wrapper(mrb_int cls_id, int fd);
int  sp_Socket_connect_raw(sp_Socket *s, const char *sa);
int  sp_Socket_bind_raw(sp_Socket *s, const char *sa);
int  sp_Socket_listen_raw(sp_Socket *s, int backlog);
int  sp_Socket_accept_raw(sp_Socket *s);
int  sp_Socket_accept_one_raw(sp_RbVal sock_val);
int  sp_Socket_send_raw(sp_Socket *s, const char *buf, int flags);
int  sp_Socket_sendto_raw(sp_Socket *s, const char *buf, int flags,
                           const char *sa);

/* ---- Addrinfo high-level op wrappers ---- */
sp_Socket *sp_Addrinfo_connect(sp_Addrinfo *a);
sp_Socket *sp_Addrinfo_bind(sp_Addrinfo *a);
int        sp_Addrinfo_listen(sp_Addrinfo *a, int backlog);

/* ---- getaddrinfo / sockaddr wrap helpers ---- */
sp_RbVal     sp_Socket_getaddrinfo_wrap(const char *node, const char *service,
                                         int family, int socktype, int protocol, int flags);
const char *sp_Socket_pack_in_wrap(const char *host, int port);
const char *sp_Socket_pack_un_wrap(const char *path);
const char *sp_Socket_in_addr_wrap(const char *sa);
int         sp_Socket_in_port_wrap(const char *sa);
const char *sp_Socket_un_path_wrap(const char *sa);
int  sp_Socket_family_wrap(const char *sa);
sp_Addrinfo *sp_Addrinfo_new_wrapper(mrb_int cls_id, int family, int socktype,
                                     int protocol, const char *bin);
sp_Addrinfo *sp_Addrinfo_new_canon_wrapper(mrb_int cls_id, int family, int socktype,
                                           int protocol, const char *bin,
                                           const char *canonname);
sp_Addrinfo *sp_Addrinfo_from_bin_auto(mrb_int cls_id, const char *bin, int len);
sp_RbVal     sp_Socket_getaddrinfo_strings(const char *node, const char *service,
                                            int family, int socktype, int protocol, int flags);
sp_RbVal     sp_Socket_socketpair_fds(int family, int type, int proto);
sp_Socket   *sp_Socket_accept_one(mrb_int cls_id, sp_Socket *s);

/* ---- Addrinfo instance accessors ---- */
int  sp_Addrinfo_afamily(sp_Addrinfo *a);
int  sp_Addrinfo_pfamily(sp_Addrinfo *a);
int  sp_Addrinfo_socktype(sp_Addrinfo *a);
int  sp_Addrinfo_protocol(sp_Addrinfo *a);
const char *sp_Addrinfo_canonname(sp_Addrinfo *a);
const char *sp_Addrinfo_to_sockaddr(sp_Addrinfo *a);   /* binary sockaddr */
int  sp_Addrinfo_ipv4_p(sp_Addrinfo *a);
int  sp_Addrinfo_ipv6_p(sp_Addrinfo *a);
int  sp_Addrinfo_unix_p(sp_Addrinfo *a);
int  sp_Addrinfo_ip_p(sp_Addrinfo *a);
const char *sp_Addrinfo_ip_address(sp_Addrinfo *a);
int         sp_Addrinfo_ip_port(sp_Addrinfo *a);
const char *sp_Addrinfo_unix_path(sp_Addrinfo *a);

#endif /* SP_SOCKET_H */
