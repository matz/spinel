/* sp_net.h -- POSIX TCP / poll / process / shell primitives for
 * single-host, HTTP-shaped Spinel runtimes.
 *
 * Pure C, no spinel-runtime dependency. Archived into libspinel_rt.a
 * alongside sp_crypto / sp_bigint -- a vendored, audit-sized helper
 * that ships with spinel so frameworks (tep, roundhouse, ...) build
 * their HTTP layer on top instead of fork-and-modifying. Same
 * precedent + spirit as sp_crypto (#514): small enough to read in one
 * sitting, no OpenSSL / libsodium. (TLS is deliberately NOT here --
 * an optional sp_net_tls unit will layer over these fds so the core
 * stays dependency-light; see matz/spinel#1054.)
 *
 * Conventions
 * -----------
 * - fd-based: every call takes/returns a raw socket fd; the framework
 *   owns lifetime.
 * - Static return buffers (per-function) -- copy on the caller side if a
 *   value must outlive the next call, same as sp_crypto. The buffers are
 *   per-worker in the threaded build, so two green threads on two OS
 *   workers do not share one; each still holds only its own last result.
 * - String inputs are NUL-terminated.
 *
 * Naming: all exported symbols use the `sp_net_` prefix.
 */
#ifndef SP_NET_H
#define SP_NET_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- graceful shutdown (prefork servers) ----
 * install_term_handlers arms SIGTERM/SIGINT to set a flag (SA_RESETHAND
 * so a second signal kills immediately); shutdown_requested reads it.
 * sp_net_accept honors the flag so a blocking accept loop can break
 * cleanly. */
int sp_net_install_term_handlers(void);
int sp_net_shutdown_requested(void);

/* ---- TCP socket lifecycle ----
 * listen: bind+listen on `port` (INADDR_ANY), backlog SOMAXCONN;
 *   reuseport!=0 sets SO_REUSEPORT for kernel accept load-balancing
 *   across prefork workers. Disables Nagle + ignores SIGPIPE. Returns
 *   the listen fd.
 * accept: blocking accept; returns -1 if a term signal arrived
 *   (checked before blocking and on EINTR), else the connection fd.
 * accept_nb: non-blocking accept; -1 with errno EAGAIN/EWOULDBLOCK if
 *   nothing pending (listen fd must be sp_net_set_nonblock'd first).
 *   accept / accept_nb set TCP_NODELAY on the returned connection fd.
 * connect: outbound TCP to host:port via getaddrinfo (IP or DNS),
 *   Nagle off. Returns the connected fd or -1.
 * close: close(fd). set_nonblock: flip O_NONBLOCK on.
 * set_nodelay: disable Nagle on a connection fd (called for you by
 *   accept/accept_nb/connect; exposed for fds obtained elsewhere). */
int sp_net_listen(int port, int reuseport);
int sp_net_listen_host(const char *host, int port, int backlog);
int sp_net_listen_all(const char *host, int port, int *fds, int cap, const char **op);
int sp_net_local_port(int fd);
int sp_net_sock_ip(int fd, int peer, char *ipbuf, int cap);
int sp_net_sock_host(int fd, int peer, char *hostbuf, int cap);
int sp_net_sock_name(int fd, int peer, void *out, int cap);
int sp_net_accept(int sfd);
int sp_net_accept_unless(int sfd, const unsigned char *cancel);
int sp_net_accept_nb(int sfd);
/* The peer IP of a connected fd as text, "" when it has none. */
const char *sp_net_peer_ip(int fd);
int sp_net_connect(const char *host, int port);
/* connect an existing fd to a packed sockaddr, waiting as connect does */
int sp_net_connect_sa(int fd, const void *sa, int len);

/* connect with a deadline: gives up after timeout_s seconds and returns -1
 * (a negative timeout_s waits as connect does). The deadline covers every
 * address the host resolves to; the name lookup itself is not bounded.
 * Under SP_THREADS the green thread parks with the deadline
 * (sp_sched_wait_io_timeout), and the returned fd is non-blocking as
 * connect's is there. take_connect_timeout tells a timeout apart from a
 * refusal or a failed lookup: 1 when the calling worker's last
 * connect_timeout gave up on the deadline, else 0. Reading it clears it.
 * connect_timeout_ip then names the address that connect was waiting on,
 * as sock_ip names a socket's: its IP as text into ipbuf, and the port
 * returned (-1 when there is none). */
int sp_net_connect_timeout(const char *host, int port, double timeout_s);
int sp_net_take_connect_timeout(void);
int sp_net_connect_timeout_ip(char *ipbuf, int cap);

/* ---- UDP ----
 * udp_open: an unbound SOCK_DGRAM fd. bind/connect attach it to a local /
 * remote address. send_to with a NULL host uses the connected peer.
 * recv_from reads one datagram and reports the sender. */
int sp_net_udp_open(int family);
int sp_net_fd_family(int fd);
int sp_net_udp_bind(int fd, const char *host, int port);
int sp_net_udp_connect(int fd, const char *host, int port);
int sp_net_udp_send_to(int fd, const char *data, int len, int flags, const char *host, int port);
int sp_net_udp_recv_from(int fd, char *buf, int cap, char *ipbuf, int ipcap, int *port_out);

/* ---- UNIX-domain stream sockets ---- */
int sp_net_unix_listen(const char *path, int backlog);
int sp_net_unix_connect(const char *path);
int sp_net_unix_path(int fd, int peer, char *buf, int cap);

/* ---- Socket class methods ---- */
int sp_net_gethostname(char *buf, int cap);
int sp_net_socketpair(int domain, int type, int protocol, int fds[2]);
int sp_net_socket(int domain, int type, int protocol);
/* The EAI_* code of the lookup a -1 above failed on, or 0 when it was not
   a lookup that failed; reading it clears it. */
int sp_net_take_resolution_error(void);
int sp_net_getaddrinfo_at(const char *host, int port, int want_family, int socktype, int idx,
                          int *family, int *stype, int *proto,
                          char *ipbuf, int ipcap, int *port_out);

/* ---- socket options ----
 * The integer-valued options, which is what Ruby programs reach for
 * (SO_REUSEADDR, SO_KEEPALIVE, TCP_NODELAY, SO_RCVBUF, ...). */
int sp_net_setsockopt_int(int fd, int level, int optname, int value);
int sp_net_getsockopt_int(int fd, int level, int optname);
int sp_net_shutdown(int fd, int how);
int sp_net_close(int fd);
int sp_net_set_nonblock(int fd);
void sp_net_set_nodelay(int fd);

/* ---- TCP I/O ----
 * recv_some: up to maxlen bytes from one read. recv_all: read until
 * EOF or max_bytes. Both return a static, NUL-terminated buffer
 * (empty on error/EOF). write_str: write the full NUL-terminated
 * string; write_bytes: binary variant (explicit length, NUL-safe).
 * Return 0 on success, -1 on failure.
 *
 * Binary-safe recv: a NUL-terminated buffer can't carry an embedded
 * 0x00 (WebSocket frames do). Declaring recv_some/recv_all with the
 * FFI `:binstr` return mode builds the result String from the exact
 * byte count published in sp_ffi_bin_len instead of strlen, so binary
 * payloads survive. The same C functions serve both `:str` and
 * `:binstr` callers; sp_ffi_bin_len (declared in sp_alloc.h) holds the
 * last recv's byte count. */
const char *sp_net_recv_some(int fd, int maxlen);
const char *sp_net_recv_all(int fd, int max_bytes);

/* recv_some with a deadline: waits at most timeout_s seconds for the first
 * byte (a negative timeout_s waits forever, as recv_some does). It parks the
 * green thread with the deadline (sp_sched_wait_io_timeout) rather than in a
 * separate readiness wait before each read, and works the same on a blocking
 * fd. An empty result is EOF, an error or a timeout; take_recv_timeout tells
 * them apart: 1 when the calling worker's last recv_some_timeout gave up on
 * the deadline, else 0. Reading it clears it. */
const char *sp_net_recv_some_timeout(int fd, int maxlen, double timeout_s);
int         sp_net_take_recv_timeout(void);

int         sp_net_write_str(int fd, const char *s);
int         sp_net_write_bytes(int fd, const char *data, int n);

/* Non-blocking write: takes what the socket will accept now and answers how
 * much that was, so the caller keeps the backpressure policy. write_str /
 * write_bytes above block until everything is written, which for fan-out
 * means one slow peer delays every other. Returns >0 bytes accepted, 0 when
 * the peer is alive but its buffer is full (wait for WRITE readiness and call
 * again with the remainder), -1 when the connection is gone. */
int         sp_net_write_partial(int fd, const char *data, int n);

/* ---- poll(2) ----
 * reset clears the slot table; add registers (fd, mode_bits) where
 * 1=READ, 2=WRITE and returns the slot index; run blocks up to
 * timeout_ms (-1 = forever, 0 = peek) and returns the ready count;
 * ready(slot) returns the mode bits that fired (POLLHUP/POLLERR fold
 * into READ).
 *
 * The set has no fixed ceiling -- it grows with the number of fds added
 * between resets. add() answers -1 only when the allocator refuses, which
 * it also reports on stderr, since a caller cannot tell that apart from
 * "not registered this round" by the return value alone.
 *
 * The set is rebuilt every tick (reset/add/.../run), which costs the caller
 * O(fds) per tick whatever the backend underneath. The persistent-registration
 * calls below cost O(events) instead; prefer them for a connection-holding
 * server. */
int sp_net_poll_reset(void);
int sp_net_poll_add(int fd, int mode_bits);
int sp_net_poll_run(int timeout_ms);
int sp_net_poll_ready(int slot);

/* ---- poll(2), persistent registration ----
 * The four above rebuild the set every tick, so a caller with N parked fds
 * does O(N) work per tick however few of them are ready. These register once
 * per connection instead: wait() answers how many fds fired, and
 * event_fd/event_mode(i) read the i'th of those, so the caller's work is
 * proportional to EVENTS rather than to connections.
 *
 * register(fd, mode_bits) adds or, if fd is already registered, updates it;
 * modify changes the interest (e.g. adding WRITE while a send is pending);
 * unregister drops it. mode_bits are 1=READ, 2=WRITE as above, and
 * event_mode folds POLLHUP/POLLERR into READ as ready() does. register /
 * modify / unregister answer 0, or -1 for an unregistered fd or an allocator
 * refusal (which is also reported on stderr). wait returns the ready count,
 * or -1 if shutdown was requested. registered() is the current set size.
 *
 * The events wait() reports are a snapshot, so unregistering an fd while
 * walking them -- what a server does on every EOF -- does not disturb the
 * walk. Both sets are per-worker, like the recv buffers: a green thread is
 * pinned to its worker, so it always polls the fds it registered, and two
 * threads running their own loops do not share one set.
 *
 * The backend is still poll(2). A caller written to this contract does not
 * change again if epoll/kqueue is put underneath -- which is the point of
 * the shape rather than of the syscall. Use these OR the four above, not
 * both: they keep separate sets. (matz/spinel#4103) */
int sp_net_poll_register(int fd, int mode_bits);
int sp_net_poll_modify(int fd, int mode_bits);
int sp_net_poll_unregister(int fd);
int sp_net_poll_registered(void);
int sp_net_poll_wait(int timeout_ms);
int sp_net_poll_event_fd(int i);
int sp_net_poll_event_mode(int i);

/* ---- process (prefork) ----
 * fork: 0 in child, pid>0 in parent, -1 on failure. exit: _exit(status)
 * (never returns; int for FFI symmetry). getpid. wait_any: reap one
 * child, returns its pid or -1 when none remain. */
int sp_net_fork(void);
int sp_net_exit(int status);
int sp_net_getpid(void);
int sp_net_wait_any(void);

/* ---- shell ----
 * Run `cmd` via popen("r"), capture stdout up to max_bytes (capped at
 * the internal buffer). Returns a static NUL-terminated buffer. */
const char *sp_net_shell_capture(const char *cmd, int max_bytes);

/* ---- packed sockaddr ----
 * The byte form Socket.sockaddr_in / Addrinfo#to_sockaddr produce and
 * connect_nonblock(sockaddr) consumes. Each writes into the caller's buffer
 * and returns the packed length, or -1. sp_net_unpack_sockaddr_in reads one
 * back, returning the port and writing the numeric address into ipbuf. */
int sp_net_pack_sockaddr_in(const char *host, int port, void *out, int cap);
int sp_net_pack_sockaddr_un(const char *path, void *out, int cap);
int sp_net_unpack_sockaddr_in(const void *sa, int salen, char *ipbuf, int cap);

#ifdef __cplusplus
}
#endif

#endif /* SP_NET_H */
