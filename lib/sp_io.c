/* sp_io.c -- File / IO handle ops in libspinel_rt.a.
 *
 * The allocation-free handle ops (open / pipe / fdopen / write / close /
 * closed? / puts / print / flush / eof?); the string-returning readers
 * (gets / read / read_n / path) stay inline in spinel_rt.h.
 *
 * Self-contained: includes sp_io.h (the sp_File layout) + sp_gc.h
 * (sp_mark_string), not spinel_rt.h. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* pipe2 */
#endif
#include "sp_io.h"
#include "sp_gc.h"   /* sp_mark_string */
#include <stdlib.h>
#include <stddef.h>   /* offsetof, for the static-stream layout assertion */
#include <string.h>
#include <unistd.h>   /* pipe, isatty */
#include <sys/stat.h> /* stat() for the File predicates */
#include <sys/ioctl.h> /* TIOCGWINSZ for #winsize */
#if !defined(__wasi__) && !defined(_WIN32)
#include <termios.h>   /* io/console's IO#raw / IO#noecho */
#endif
#include <sys/socket.h> /* the Socket:: constants */
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <sys/time.h> /* utimes() for File.utime */
#include <errno.h>
#include <limits.h>  /* INT_MAX for the socket int arguments */
#include <fcntl.h>   /* fcntl flags for #close_on_exec?, #fcntl */
#include <poll.h>    /* POLLIN for the socket read park */
#include <time.h>

/* Per-call "do not block" for send(2). Linux honours it; Darwin and the BSDs
   accept the flag and sleep anyway once the send buffer is full, which is why
   sp_sock_room below exists. Where the flag is missing entirely it is 0 and
   the room cap carries the socket on its own. */
#ifdef MSG_DONTWAIT
#define SP_MSG_DONTWAIT MSG_DONTWAIT
#else
#define SP_MSG_DONTWAIT 0
#endif

/* Bytes the send buffer can take right now, or -1 when the kernel will not
   say. Capping a send at this is what keeps a blocking socket's write out of
   the kernel's sleep: poll(POLLOUT) answers "there is room", not "there is
   room for all of n", so a 64 KB write into a nearly full buffer blocks even
   though the descriptor was reported writable. */
static long sp_sock_room(int fd) {
  int capacity = 0, queued = 0;
  socklen_t len = sizeof capacity;
  if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &capacity, &len) != 0 || capacity <= 0) return -1;
#if defined(SO_NWRITE)          /* Darwin: bytes not yet handed to the peer */
  len = sizeof queued;
  if (getsockopt(fd, SOL_SOCKET, SO_NWRITE, &queued, &len) != 0) return -1;
#elif defined(TIOCOUTQ)         /* Linux (SIOCOUTQ): same question, via ioctl */
  if (ioctl(fd, TIOCOUTQ, &queued) != 0) return -1;
#else
  return -1;
#endif
  long room = (long)capacity - (long)queued;
  return room > 0 ? room : 0;
}
#if !defined(__APPLE__) && !defined(__GLIBC__)
#include <stdio_ext.h>  /* musl __freadahead: pending stdio read-buffer bytes */
#endif

/* Provided by the generated TU / libspinel_rt.a. */
extern void *sp_gc_alloc(size_t sz, void (*fin)(void *), void (*scn)(void *));
extern SP_NORETURN void sp_raise_cls(const char *cls, const char *msg);
extern const char *sp_errno_class_name(int e);   /* lib/sp_exc.c: the Errno:: class for a C errno */
extern const char *sp_sprintf(const char *fmt, ...);

typedef struct sp_io_timeout {
  sp_RbVal value;
  double seconds;
} sp_io_timeout;

static void sp_io_timeout_scan(void *p) {
  sp_mark_rbval(((sp_io_timeout *)p)->value);
}

static void sp_io_mark_static_timeouts(void) {
  sp_gc_mark(SP_ATOMIC_LOAD(&sp_io_stdout()->timeout, __ATOMIC_ACQUIRE));
  sp_gc_mark(SP_ATOMIC_LOAD(&sp_io_stderr()->timeout, __ATOMIC_ACQUIRE));
  sp_gc_mark(SP_ATOMIC_LOAD(&sp_io_stdin()->timeout, __ATOMIC_ACQUIRE));
}

sp_RbVal sp_File_timeout(sp_File *f) {
  if (f && f->mode && (strcmp(f->mode, "stat") == 0 || strcmp(f->mode, "lstat") == 0))
    sp_raise_cls("NoMethodError", "undefined method 'timeout' for an instance of File::Stat");
  sp_io_timeout *timeout = f ? SP_ATOMIC_LOAD(&f->timeout, __ATOMIC_ACQUIRE) : NULL;
  return timeout ? timeout->value : sp_box_nil();
}

double sp_io_timeout_seconds(sp_File *f) {
  sp_io_timeout *timeout = f ? SP_ATOMIC_LOAD(&f->timeout, __ATOMIC_ACQUIRE) : NULL;
  if (!timeout) return -1.0;
  if (timeout->value.tag == SP_TAG_BOOL)
    sp_raise_cls("TypeError", "can't convert FalseClass into time interval");
  return timeout->seconds;
}

sp_File *sp_io_set_timeout(sp_File *f, sp_RbVal value, double seconds) {
  SP_GC_ROOT(f); SP_GC_ROOT_RBVAL(value);
  if (!f) sp_raise_cls("IOError", "uninitialized stream");
  sp_io_timeout *timeout = NULL;
  SP_GC_ROOT(timeout);
  if (value.tag != SP_TAG_NIL) {
    timeout = (sp_io_timeout *)sp_gc_alloc(sizeof(*timeout), NULL, sp_io_timeout_scan);
    timeout->value = value;
    timeout->seconds = seconds;
  }
  sp_gc_wb(f);
  if (((const unsigned char *)f)[-1] == 0xff)
    SP_ATOMIC_STORE(&sp_gc_mark_io_timeouts_hook, sp_io_mark_static_timeouts, __ATOMIC_RELEASE);
  SP_ATOMIC_STORE(&f->timeout, timeout, __ATOMIC_RELEASE);
  return f;
}

static double sp_io_monotonic(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double sp_io_deadline(sp_File *f) {
  double seconds = sp_io_timeout_seconds(f);
  return seconds < 0.0 ? -1.0 : sp_io_monotonic() + seconds;
}

/* The finalizer honors autoclose: an IO whose fd the program disowned
   (io.autoclose = false after wrapping the fd itself) flushes and abandons
   the FILE rather than closing the caller's descriptor -- the FILE structure
   leaks, which is the price of libc having no fdclose(3); the for_fd
   autoclose:false path wraps a dup(2) instead and never pays it (#4208). */
static void sp_File_fin(void *p) {
  sp_File *f = (sp_File *)p;
  if (!f->fp || f->closed) return;   /* closed: the FILE is the shared sentinel */
  if (f->no_autoclose && !f->fno_plus1) { fflush(f->fp); f->fp = NULL; return; }
  fclose(f->fp); f->fp = NULL;
}
static void sp_File_scan(void *p) { sp_File *f = (sp_File *)p; if (f->path) sp_mark_string(f->path); if (f->mode) sp_mark_string(f->mode); sp_io_timeout *timeout = SP_ATOMIC_LOAD(&f->timeout, __ATOMIC_ACQUIRE); if (timeout) sp_gc_mark(timeout); }

/* The two-argument form is the permission form with CRuby's default bits:
   the same mode scan (which refuses "rx"), O_CLOEXEC, and the errno-named
   error, rather than fopen's looser mode and a flat ENOENT. */
sp_File *sp_File_open(const char *path, const char *mode) {
  return sp_File_open_perm(path, mode ? mode : "r", sp_oint_nil());
}

#if !defined(__linux__) && !defined(_WIN32)
static int sp_io_set_cloexec(int fd) {
  int fl = fcntl(fd, F_GETFD);
  if (fl < 0) return -1;
  return (fl & FD_CLOEXEC) ? 0 : fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}
#endif

/* Returns 0 on success, -1 on error with errno set. Both ends are
   close-on-exec, as CRuby's are. Without pipe2 the flag is set after pipe(),
   so a spawn on another worker in between can still inherit them. */
int sp_io_make_pipe(int fds[2]) {
#if defined(__linux__)
  return pipe2(fds, O_CLOEXEC);
#elif defined(_WIN32)
  return pipe(fds);
#else
  if (pipe(fds) != 0) return -1;
  if (sp_io_set_cloexec(fds[0]) != 0 || sp_io_set_cloexec(fds[1]) != 0) {
    int e = errno;
    close(fds[0]);
    close(fds[1]);
    errno = e;
    return -1;
  }
  return 0;
#endif
}

/* IO.pipe end: wrap a raw pipe fd in a GC-managed sp_File so the
   sp_File_* I/O ops work on it. Same finalizer/scan as sp_File_open. */
/* owns_fd: the descriptor is this handle's to release when fdopen fails --
   true for one open(2) or pipe(2) just made, false for IO.for_fd, whose fd
   belongs to the program (closing its stdout on a bad mode lost the output). */
sp_File *sp_io_fdopen_ex(int fd, const char *mode, int owns_fd) {SP_GC_ROOT_STR(mode);
  sp_File *f = (sp_File *)sp_gc_alloc(sizeof(sp_File), sp_File_fin, sp_File_scan);
  f->fp = fdopen(fd, mode ? mode : "r");
  if (!f->fp) {
    if (owns_fd) close(fd);
    sp_raise_cls("IOError", "fdopen failed");
    return NULL;
  }
  f->path = NULL;
  f->mode = mode;
  f->lineno = 0;
  return f;
}
sp_File *sp_io_fdopen(int fd, const char *mode) { return sp_io_fdopen_ex(fd, mode, 1); }

/* BasicSocket.do_not_reverse_lookup: true unless the program changes it; a
   socket takes it when it is made, and keeps its own copy after that */
sp_bool sp_sock_dnrl_default = 1;

/* Wrap a socket fd (#2922). The FILE* serves the buffered READ side (gets and
   friends need lookahead); every write bypasses stdio straight to write(2) --
   see sp_sock_write below -- so a response is on the wire immediately, like
   CRuby sockets' sync = true default. `kind` lands in ->mode for #class. */
sp_File *sp_io_fdopen_sock(int fd, const char *kind) {SP_GC_ROOT_STR(kind);
  if (fd < 0) sp_raise_cls("Errno::ECONNREFUSED", "Connection refused");
  /* The scheduler-aware accept/connect helpers may hand us a non-blocking
     fd; the stdio read side must BLOCK (fgets treats EAGAIN as EOF), so
     clear O_NONBLOCK. Green threads still yield: every read entry point
     parks on sp_io_wait_readable below before touching the stream. */
  { int fl = fcntl(fd, F_GETFL); if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK); }
  sp_File *f = (sp_File *)sp_gc_alloc(sizeof(sp_File), sp_File_fin, sp_File_scan);
  f->fp = fdopen(fd, "r+");
  if (!f->fp) { sp_raise_cls("IOError", "fdopen failed"); return NULL; }
  f->path = NULL;
  f->mode = kind;
  f->lineno = 0;
  f->is_sock = 1; f->rev_lookup = !sp_sock_dnrl_default;
  return f;
}

/* Park the calling green thread until the handle has readable data, so a
   blocking fgets/fread does not pin its worker while the peer is idle. A
   no-op when stdio already buffered data (waiting then would stall on the
   WIRE while the answer sits in the buffer), and in the single-threaded
   build.

   WHICH handles: the ones whose read can block indefinitely. This was the
   socket path and asked `is_sock`, so a PIPE went straight into fread and
   blocked its OS worker in the kernel -- and the thread that would write to
   that pipe is often a green thread on the very same worker, so it had
   nowhere to run: two threads passing a byte back and forth never completed
   on SPINEL_WORKERS=1 (#4307). A character device (a tty, /dev/urandom) is
   the same shape. A regular file is always ready and pays only the one
   fstat, once per handle. */
/* Bytes stdio has already buffered for this stream. They are readable without
   a read(2), so a drain of exactly this many cannot block -- which is what
   lets a slurp fill from a pipe without ever sitting in the kernel. */
size_t sp_io_stdio_buffered(FILE *fp) {
  if (!fp) return 0;
#if defined(__GLIBC__)
  return fp->_IO_read_end > fp->_IO_read_ptr ? (size_t)(fp->_IO_read_end - fp->_IO_read_ptr) : 0;
#elif defined(__APPLE__)
  return fp->_r > 0 ? (size_t)fp->_r : 0;
#else
  return __freadahead(fp);   /* musl */
#endif
}

/* A thread reading or writing a handle's descriptor while another thread
   closes it (#4546's shape: a connection's two pumps, either of which may
   close both ends). The close used to fclose at once, and a reader that had
   loaded the FILE just before it then read the freed FILE's buffer pointers,
   or called read(2) on a descriptor number the next accept or open had
   already been given. So the reads and writes that go to the descriptor
   take a use of the handle around it (sp_io_use .. sp_io_unuse); the close
   marks the handle, and the FILE is released by whichever of the closer
   and those users leaves last. A use spans no park but accept's, which the
   close cancels: a parked thread is woken by the close (sp_sched_ev_forget)
   and re-tests the closed flag. */
#define SP_IO_CLOSING 0x80000000u
#define SP_IO_RETIRED 0x40000000u   /* with CLOSING: the FILE is released */
#define SP_IO_REOPENING 0x20000000u /* with both: a #reopen is giving it a new one */
/* The stream, with a use taken; NULL when a close has begun. */
static FILE *sp_io_try_use(sp_File *f) {
  unsigned int u = SP_ATOMIC_LOAD(&f->uses, __ATOMIC_RELAXED);
  do {
    if (u & SP_IO_CLOSING) return NULL;
  } while (!SP_ATOMIC_CAS(&f->uses, &u, u + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
  return SP_ATOMIC_LOAD(&f->fp, __ATOMIC_RELAXED);
}
/* The same, raising CRuby's IOError for a close from another thread. */
static FILE *sp_io_use(sp_File *f) {
  FILE *fp = sp_io_try_use(f);
  if (!fp) sp_raise_cls("IOError", "stream closed in another thread");
  return fp;
}
/* Release the FILE of a closed handle: from here on the handle carries the
   /dev/null sentinel (see sp_File.closed). */
static int sp_io_retire(sp_File *f) {
  FILE *fp = SP_ATOMIC_LOAD(&f->fp, __ATOMIC_RELAXED);
  SP_ATOMIC_STORE(&f->fp, sp_io_closed_sentinel(), __ATOMIC_RELAXED);
  /* autoclose=false on an IO that wraps the fd itself: flush and abandon
     the FILE (see sp_File_fin); a for_fd wrapper holds a dup, so its
     fclose never reaches the caller's descriptor (#4208) */
  if (f->no_autoclose && !f->fno_plus1) { fflush(fp); return 0; }
  return fclose(fp) == 0 ? 0 : -1;
}
/* Drop a use. The last one out of a closed handle releases the FILE and
   answers fclose's result; errno is the caller's to keep. */
static int sp_io_unuse(sp_File *f) {
  if (SP_ATOMIC_SUB_FETCH(&f->uses, 1, __ATOMIC_ACQ_REL) != SP_IO_CLOSING) return 0;
  int r = sp_io_retire(f);
  /* only now may #reopen give the handle a new stream (sp_File_reopen) */
  SP_ATOMIC_STORE(&f->uses, SP_IO_CLOSING | SP_IO_RETIRED, __ATOMIC_RELEASE);
  return r;
}

int sp_io_readiness_fd(sp_File *f, int *buffered) {
  FILE *fp = sp_io_use(f);
  int fd = fileno(fp);
  if (buffered) *buffered = sp_io_stdio_buffered(fp) > 0;
  sp_io_unuse(f);
  return fd;
}

/* Can this handle's I/O block indefinitely? Asked once per handle, from an
   fstat on first use: a FIFO, a socket and a character device can; a regular
   file is always ready. A handle closed before it was ever asked answers yes,
   so the caller goes on to the park or the use that raises. */
static int sp_io_parkable(sp_File *f) {
  unsigned char k = SP_ATOMIC_LOAD(&f->park, __ATOMIC_RELAXED);
  if (!k) {
    FILE *fp = sp_io_try_use(f);
    if (!fp) return 1;
    struct stat st;
    int pfd = fileno(fp);
    k = (f->is_sock ||
         (pfd >= 0 && fstat(pfd, &st) == 0 &&
          (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode) || S_ISCHR(st.st_mode)))) ? 2 : 1;
    sp_io_unuse(f);
    SP_ATOMIC_STORE(&f->park, k, __ATOMIC_RELAXED);
  }
  return k == 2;
}

static void sp_io_wait_deadline(sp_File *f, short events, double deadline) {
  SP_GC_ROOT(f);
  int fd = fileno(sp_io_use(f));
  sp_io_unuse(f);
  for (;;) {
    struct pollfd p = { .fd = fd, .events = events, .revents = 0 };
    int ready = SP_SIG_BLOCKING(poll(&p, 1, 0));
    if (SP_ATOMIC_LOAD(&f->closed, __ATOMIC_RELAXED))
      sp_raise_cls("IOError", "stream closed in another thread");
    if (ready > 0) return;
    if (ready < 0 && errno != EINTR && errno != EAGAIN)
      sp_file_raise_errno("poll", "");
    double remaining = deadline < 0.0 ? -1.0 : deadline - sp_io_monotonic();
    if (deadline >= 0.0 && remaining <= 0.0)
      sp_raise_cls("IO::TimeoutError", "Blocking operation timed out!");
#ifdef SP_THREADS
    extern int sp_sched_wait_io_timeout_unless(int, short, double, const unsigned char *);
    ready = sp_sched_wait_io_timeout_unless(fd, events, remaining, &f->closed);
    if (SP_ATOMIC_LOAD(&f->closed, __ATOMIC_RELAXED))
      sp_raise_cls("IOError", "stream closed in another thread");
    if (!ready && deadline >= 0.0)
      sp_raise_cls("IO::TimeoutError", "Blocking operation timed out!");
#else
    extern void sp_Thread_pass(void);
    p.revents = 0;
    (void)SP_SIG_BLOCKING(poll(&p, 1, 1));
    sp_Thread_pass();
#endif
  }
}

static void sp_io_write_wait(sp_File *f, double deadline) {
  if (deadline < 0.0) sp_io_wait_writable(f);
  else sp_io_wait_deadline(f, POLLOUT, deadline);
}

/* Park until the descriptor can take bytes. A write into a full pipe or socket
   buffer sits in the kernel holding the OS worker, and the READER that would
   drain it is often a green thread on that same worker -- so it never runs and
   the two deadlock, the write-side twin of the read park. Only for the handles
   whose peer is that reader: a socket, and the write end of an IO.pipe (which
   spinel marks sync and leaves unbuffered). A file and the standard streams
   keep their plain path, and their write cost with it. */
void sp_io_wait_writable(sp_File *f) {SP_GC_ROOT(f);
  if (!f || !SP_ATOMIC_LOAD(&f->fp, __ATOMIC_RELAXED) || !sp_io_parkable(f)) return;
  int wfd = fileno(sp_io_use(f));
  sp_io_unuse(f);
  if (wfd < 0) return;
  { struct pollfd wp; wp.fd = wfd; wp.events = POLLOUT; wp.revents = 0;
    if (SP_SIG_BLOCKING(poll(&wp, 1, 0)) > 0) return; }   /* room already: no park */
#ifdef SP_THREADS
  {
    extern int sp_sched_wait_io_unless(int fd, short events, const unsigned char *cancel);
    sp_sched_wait_io_unless(wfd, POLLOUT, &f->closed);
    if (SP_ATOMIC_LOAD(&f->closed, __ATOMIC_RELAXED)) sp_raise_cls("IOError", "stream closed in another thread");   /* see sp_io_wait_readable */
  }
#else
  {
    extern void sp_Thread_pass(void);
    struct pollfd p;
    p.fd = wfd; p.events = POLLOUT;
    for (;;) {
      p.revents = 0;
      int r = SP_SIG_BLOCKING(poll(&p, 1, 1));
      if (r > 0 || (r < 0 && errno != EINTR && errno != EAGAIN)) return;
      sp_Thread_pass();
    }
  }
#endif
}

/* The park on a bare descriptor. `closed` is the owning handle's closed flag
   when there is one: a close from another thread between the poll below and
   the park's registration is then seen under the scheduler lock, instead of
   the registration going in after the close dropped it (a read with no
   deadline waited forever, one macOS run in three of the #4546 test). */
static void sp_io_park_fd_readable(int fd, const unsigned char *closed) {
  if (fd < 0) return;
  /* Already readable: answer from one poll rather than a park. The park is a
     monitor round trip -- register the waiter, write the self-pipe to break
     the monitor out of ITS poll, wait to be requeued and rescheduled -- and
     paying that when the bytes are already there is most of the cost in the
     shape that reads right after an IO.select says the fd is ready. */
  { struct pollfd rp; rp.fd = fd; rp.events = POLLIN; rp.revents = 0;
    if (SP_SIG_BLOCKING(poll(&rp, 1, 0)) > 0) return; }
#ifdef SP_THREADS
  {
    extern int sp_sched_wait_io_unless(int fd, short events, const unsigned char *cancel);
    sp_sched_wait_io_unless(fd, POLLIN, closed);
  }
#else
  (void)closed;
  /* Cooperative build: a blocking read here would stall EVERY green thread
     (the peer that will produce our data included). Poll with a short
     timeout and hand the scheduler to the other threads until readable. */
  {
    extern void sp_Thread_pass(void);
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    for (;;) {
      p.revents = 0;
      int r = SP_SIG_BLOCKING(poll(&p, 1, 1));
      if (r > 0 || (r < 0 && errno != EINTR && errno != EAGAIN)) return;
      sp_Thread_pass();
    }
  }
#endif
}
/* The descriptor form, for a pipe the runtime holds without a handle (a
   backtick's child): the same park, on the bare fd. */
void sp_io_wait_fd_readable(int fd) { sp_io_park_fd_readable(fd, NULL); }
void sp_io_wait_readable(sp_File *f) {SP_GC_ROOT(f);
  if (!f || !SP_ATOMIC_LOAD(&f->fp, __ATOMIC_RELAXED)) return;
  if (!sp_io_parkable(f)) return;
  FILE *fp = sp_io_use(f);
  size_t buffered = sp_io_stdio_buffered(fp);
  int fd = fileno(fp);
  sp_io_unuse(f);
  if (buffered > 0) return;
  sp_io_park_fd_readable(fd, &f->closed);
  /* woken by a close from another thread: the handle is gone, and a read
     would touch a descriptor that is closed or already someone else's */
  if (SP_ATOMIC_LOAD(&f->closed, __ATOMIC_RELAXED)) sp_raise_cls("IOError", "stream closed in another thread");
}

/* The socket write path: straight to the descriptor, looping over short
   writes. The stdio stream is never written through, so there is nothing to
   flush and no read/write switching hazard on the shared FILE*. */
static sp_int sp_sock_write(sp_File *f, const char *s, size_t n) {
  double deadline = n ? sp_io_deadline(f) : -1.0;
  /* the descriptor under a use (sp_io_use), let go only across the parks */
  int fd = fileno(sp_io_use(f));
  size_t off = 0;
  /* A pipe's WRITE end is write-only -- nothing reads through its stream -- so
     it can be O_NONBLOCK, and then a write that would block says so instead of
     blocking. That is the cheap shape: no probe on the way in, and the park
     only on the attempt that actually could not proceed. A socket shares its
     descriptor with the stdio READ side, where fgets treats EAGAIN as EOF
     (see sp_io_fdopen_sock), so it cannot carry O_NONBLOCK -- it takes
     MSG_DONTWAIT per send(2) instead, which says the same thing for this one
     call and leaves the descriptor's flags, and the read side, untouched.
     The probe alone was not enough: it answers "there is room", not "there is
     room for all of n", so a 64 KB write into a nearly full buffer still slept
     in the kernel with the collector waiting on this worker (the #4528 shape,
     on the write side). */
  int nb = 0;
  if (!f->is_sock) {
    if (!f->wnonblock) {
      int fl = fcntl(fd, F_GETFL);
      f->wnonblock = (fl >= 0 && (fl & O_NONBLOCK ? 1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0)) ? 1 : 2;
    }
    nb = f->wnonblock == 1;
  }
  while (off < n) {
    ssize_t put;
    size_t want = n - off;
    if (!nb) {   /* frees the worker while the buffer is full */
      sp_io_unuse(f);
      sp_io_write_wait(f, deadline);
      fd = fileno(sp_io_use(f));
    }
    if (f->is_sock) {
      /* A datagram or seqpacket send is one message: shortening it would
         deliver two where the caller wrote one, and would hide the EMSGSIZE
         the kernel owes an over-long payload. Only a byte stream is capped. */
      if (!f->wstream) {
        int sotype = 0; socklen_t solen = sizeof sotype;
        f->wstream = (getsockopt(fd, SOL_SOCKET, SO_TYPE, &sotype, &solen) == 0 &&
                      sotype == SOCK_STREAM) ? 1 : 2;
      }
      /* Linux honours MSG_DONTWAIT: the send returns what fit, or EAGAIN,
         and the park below takes it from there, so the two extra syscalls
         of the room query are not paid on the hot path there. Darwin and
         the BSDs sleep despite the flag, and the query is what keeps them
         out of the kernel. */
#if defined(__linux__)
      long room = -1;
#else
      long room = f->wstream == 1 ? sp_sock_room(fd) : -1;
#endif
      /* No room: go back to the park above, which is where waiting belongs.
         It cannot spin -- POLLOUT is raised only once the free space reaches
         SO_SNDLOWAT (2048 by default), so a buffer this full does not report
         the descriptor writable and the park really parks. */
      if (room == 0) continue;
      if (room > 0 && (size_t)room < want) want = (size_t)room;
      put = send(fd, s + off, want, SP_MSG_DONTWAIT);
    }
    else {
      put = write(fd, s + off, want);
    }
    if (put < 0) {
      if (errno == EINTR) continue;
      if ((nb || f->is_sock) && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        sp_io_unuse(f);
        sp_io_write_wait(f, deadline);
        fd = fileno(sp_io_use(f));
        continue;
      }
      int e = errno;
      sp_io_unuse(f);
      errno = e;
      sp_file_raise_errno("write", f->is_sock ? "socket" : "pipe");
    }
    off += (size_t)put;
    if (put > 0 && off < n) deadline = sp_io_deadline(f);
  }
  sp_io_unuse(f);
  return (sp_int)n;
}

/* Direct write to a regular file's descriptor, bypassing the stdio
   buffer. IO#syswrite is unbuffered: the bytes must reach the file
   before the next call returns, so there is nothing to flush and no
   read/write switching hazard on the shared FILE*. Sockets and sync
   parkable handles keep sp_sock_write, which already writes straight
   to the descriptor. */
static sp_int sp_io_write_raw(sp_File *f, const char *s, size_t n) {
  int fd = fileno(f->fp);
  size_t off = 0;
  /* Flush whatever stdio still holds: a previous buffered write would
     otherwise land after this direct write and reorder the bytes. A
     flush failure is the same error path as a write failure. */
  if (fflush(f->fp) != 0) sp_file_raise_errno("write", "file");
  while (off < n) {
    ssize_t put = write(fd, s + off, n - off);
    if (put < 0) {
      if (errno == EINTR) continue;
      sp_file_raise_errno("write", "file");
    }
    off += (size_t)put;
  }
  /* Sync the FILE* position with the descriptor: the raw write loop moved
     the descriptor past where stdio thinks it is, and a zero-length write
     is still a valid sync point. Without this, a subsequent ftello,
     buffered read, or buffered write on the same stream would use the
     stale stdio offset. The stream is set to the descriptor's own offset:
     a relative seek (SEEK_CUR) counts from stdio's cached offset, which the
     raw write did not move, and moved the descriptor back to it, so the
     next syswrite overwrote this one. A descriptor with no offset (a pipe,
     a terminal) has none to share, where the relative seek raised ESPIPE. */
  off_t pos = lseek(fd, 0, SEEK_CUR);
  if (pos >= 0 && fseeko(f->fp, pos, SEEK_SET) != 0) sp_file_raise_errno("write", "file");
  return (sp_int)n;
}

/* Shared write core: `n` is the operand byte length (strlen for the
   bare-literal-safe entry, sp_str_byte_len for the binary one). */
SP_NORETURN SP_COLD void sp_io_raise_closed(void) {
  sp_raise_cls("IOError", "closed stream");
}

static sp_int sp_File_write_len(sp_File *f, const char *s, size_t n) {SP_GC_ROOT(f);SP_GC_ROOT_STR(s);
  if (f->is_sock) return sp_sock_write(f, s, n);
  /* An IO.pipe write end is sync, so setvbuf left it unbuffered and stdio
     holds nothing between calls: the raw loop is what fwrite would do anyway,
     and it can park between attempts. Every other handle keeps stdio. */
  if (f->sync_on && sp_io_parkable(f)) return sp_sock_write(f, s, n);
  return (sp_int)fwrite(s, 1, n, f->fp);
}

sp_int sp_File_write(sp_File *f, const char *s) {SP_GC_ROOT(f);SP_GC_ROOT_STR(s);
  SP_IO_OPEN(f);
  if (!s) return 0;
  return sp_File_write_len(f, s, strlen(s));
}

/* Binary-safe write: sizes the operand with the header length, so an embedded
   NUL reaches the descriptor instead of truncating the write (IO#read and
   File.read already return such bytes; only the write side dropped them).
   Reads s[-1], so it is for CODEGEN-emitted String values only -- the same
   value class String#<< / #replace already size this way. Runtime-internal
   callers and codegen's synthesized "" / "\n" literals are bare C literals
   with no marker byte and must use the plain entry above. */
sp_int sp_File_write_bin(sp_File *f, const char *s) {SP_GC_ROOT(f);SP_GC_ROOT_STR(s);
  SP_IO_OPEN(f);
  if (!s) return 0;
  return sp_File_write_len(f, s, sp_str_byte_len(s));
}

/* IO#syswrite: the same write core as sp_File_write_bin, but the byte
   length is passed in by the caller rather than read off the marker byte.
   The codegen syswrite arm sizes the operand itself -- a String source's
   length (sp_str_byte_len, so an embedded NUL reaches the descriptor) or a
   converted value's length (sp_poly_to_s + strlen) -- and hands both the
   pointer and the count to this entry, which is the single place the
   socket/pipe/stream routing decision is made. A regular file writes
   straight to the descriptor (sp_io_write_raw), bypassing the stdio
   buffer: syswrite is unbuffered, so the bytes are on disk before the
   call returns and a read back through the same or another handle sees
   them without a flush. */
sp_int sp_File_syswrite(sp_File *f, const char *s, size_t n) {SP_GC_ROOT(f);SP_GC_ROOT_STR(s);
  SP_IO_OPEN(f);
  if (!s) return 0;
  if (f->is_sock) return sp_sock_write(f, s, n);
  if (f->sync_on && sp_io_parkable(f)) return sp_sock_write(f, s, n);
  return sp_io_write_raw(f, s, n);
}

sp_bool sp_File_tty_p(sp_File *f) {
  SP_IO_OPEN(f);
  return isatty(fileno(f->fp)) ? 1 : 0;
}

sp_int sp_File_fileno(sp_File *f) {
  SP_IO_OPEN(f);
  if (f->fno_plus1) return (sp_int)(f->fno_plus1 - 1);
  return (sp_int)fileno(f->fp);
}

/* IO#winsize -> [rows, cols]. Queries the terminal; an OPEN non-tty
   (pipe/file) has no size, so CRuby raises there, but returning [0, 0] keeps
   the common "STDOUT.winsize" probe compiling and running without an
   exception path. A closed handle is the macro's IOError, as everywhere. */
sp_IntArray *sp_File_winsize(sp_File *f) {
  SP_IO_OPEN(f);
  sp_int rows = 0, cols = 0;
  struct winsize ws;
  if (ioctl(fileno(f->fp), TIOCGWINSZ, &ws) == 0) { rows = ws.ws_row; cols = ws.ws_col; }
  sp_IntArray *a = sp_IntArray_new();
  sp_IntArray_push(a, rows);
  sp_IntArray_push(a, cols);
  return a;
}

/* io/console's IO#raw { } and IO#noecho { }, which the analysis turns into
   `s = io.__sp_tty_mode(kind); begin ... ensure io.__sp_tty_restore(s) end`.
   The terminal's settings are saved and the mode set as CRuby's console.c
   sets it (set_rawmode: cfmakeraw, then ECHOE and ECHOK off; set_noecho:
   ECHO, ECHOE, ECHOK and ECHONL off), applied with TCSANOW. A handle that is
   not a terminal raises tcgetattr's Errno (ENOTTY), as CRuby's does. Answers
   the slot the saved settings sit in, for the restore; nested modes take a
   slot each. */
#if defined(__wasi__) || defined(_WIN32)
/* No termios: no handle is a terminal whose mode can be set. */
sp_int sp_File_tty_mode(sp_File *f, sp_int kind) {
  (void)kind;
  SP_IO_OPEN(f);
  sp_raise_cls(sp_errno_class_name(ENOTTY), strerror(ENOTTY));
  return 0;
}
sp_int sp_File_tty_restore(sp_File *f, sp_int slot) { (void)f; (void)slot; return 0; }
#else
#define SP_TTY_SLOTS 16
static struct termios sp_tty_saved[SP_TTY_SLOTS];
static int sp_tty_saved_fd[SP_TTY_SLOTS];
static int sp_tty_saved_used[SP_TTY_SLOTS];
sp_int sp_File_tty_mode(sp_File *f, sp_int kind) {
  SP_IO_OPEN(f);
  int fd = (int)sp_File_fileno(f);
  struct termios t;
  if (tcgetattr(fd, &t) != 0) { int e = errno; sp_raise_cls(sp_errno_class_name(e), strerror(e)); }
  int slot = -1;
  for (int i = 0; i < SP_TTY_SLOTS; i++) if (!sp_tty_saved_used[i]) { slot = i; break; }
  if (slot < 0) sp_raise_cls("RuntimeError", "io/console: terminal modes nested too deep");
  sp_tty_saved[slot] = t;
  sp_tty_saved_fd[slot] = fd;
  sp_tty_saved_used[slot] = 1;
  if (kind == 1) {
    cfmakeraw(&t);
    t.c_lflag &= ~(tcflag_t)(ECHOE | ECHOK);
  } else {
    t.c_lflag &= ~(tcflag_t)(ECHO | ECHOE | ECHOK | ECHONL);
  }
  if (tcsetattr(fd, TCSANOW, &t) != 0) {
    int e = errno;
    sp_tty_saved_used[slot] = 0;
    sp_raise_cls(sp_errno_class_name(e), strerror(e));
  }
  return (sp_int)slot;
}
/* The settings slot `slot` saved, back on the terminal they came from. */
sp_int sp_File_tty_restore(sp_File *f, sp_int slot) {
  (void)f;
  if (slot < 0 || slot >= SP_TTY_SLOTS || !sp_tty_saved_used[slot]) return 0;
  tcsetattr(sp_tty_saved_fd[slot], TCSANOW, &sp_tty_saved[slot]);
  sp_tty_saved_used[slot] = 0;
  return 0;
}
#endif

/* IO#winsize= [rows, cols] or [rows, cols, xpixel, ypixel], as CRuby's
   io/console: any other length is ArgumentError, and a handle that is not a
   terminal raises the ioctl's Errno (ENOTTY) rather than answering quietly,
   since a size that was not set is not something to carry on from. Answers
   its argument, which is what an assignment yields. A pty master is the case
   that needs it: the program sizes the terminal its child sees. */
sp_IntArray *sp_File_set_winsize(sp_File *f, sp_IntArray *size) {
  SP_IO_OPEN(f);
  sp_int n = size ? size->len : 0;
  if (n != 2 && n != 4) {
    char msg[96];
    snprintf(msg, sizeof msg, "wrong number of arguments (given %lld, expected 2 or 4)", (long long)n);
    sp_raise_cls("ArgumentError", msg);
  }
  const sp_int *v = size->data + size->start;
  struct winsize ws;
  memset(&ws, 0, sizeof ws);
  ws.ws_row = (unsigned short)v[0];
  ws.ws_col = (unsigned short)v[1];
  if (n == 4) {
    ws.ws_xpixel = (unsigned short)v[2];
    ws.ws_ypixel = (unsigned short)v[3];
  }
#ifdef TIOCSWINSZ
  if (ioctl(fileno(f->fp), TIOCSWINSZ, &ws) != 0) {
    int e = errno;
    sp_raise_cls(sp_errno_class_name(e), strerror(e));
  }
#else
  /* a platform with no terminal-size ioctl (wasm32-wasi) */
  (void)ws;
  sp_raise_cls("NotImplementedError", "winsize= is not implemented on this platform");
#endif
  return size;
}

/* The standard streams are singletons in static storage, not GC allocations.
   Reads of $stdout / $stderr compile straight to these calls and only ever
   dereference the result, so nothing scanned ever held one -- until
   `$stderr = $stdout` stored it into a real global slot, which the generated
   globals hook marks. sp_gc_mark decides what a pointer is from the byte
   before it, finds whatever .bss happens to sit there, fabricates a header at
   obj - sizeof(sp_gc_hdr) and calls the scan pointer read out of it (#3410).

   Give them the tag the collector already reads as "static storage, leave it
   alone", the same one a rodata string literal carries. The mark then returns
   before it can walk a header that was never written. The eight-byte lead-in
   puts that byte immediately before the struct; sp_File begins with a pointer,
   so the compiler adds no padding of its own, and the assertion pins it. */
typedef struct { unsigned char lead[8]; sp_File f; } sp_StaticFile;
_Static_assert(offsetof(sp_StaticFile, f) == 8, "the skip tag must sit immediately before the sp_File");
#define SP_STATIC_FILE_LEAD { 0, 0, 0, 0, 0, 0, 0, 0xff }

sp_File *sp_io_stdout(void) {
  static sp_StaticFile s = { SP_STATIC_FILE_LEAD, { NULL, "<STDOUT>", "w" } };
  if (!s.f.fp) s.f.fp = stdout;
  return &s.f;
}

sp_File *sp_io_stderr(void) {
  static sp_StaticFile s = { SP_STATIC_FILE_LEAD, { NULL, "<STDERR>", "w" } };
  if (!s.f.fp) s.f.fp = stderr;
  return &s.f;
}

sp_File *sp_io_stdin(void) {
  static sp_StaticFile s = { SP_STATIC_FILE_LEAD, { NULL, "<STDIN>", "r" } };
  if (!s.f.fp) s.f.fp = stdin;
  return &s.f;
}

sp_bool sp_io_frozen(sp_File *f) { return f ? (sp_bool)f->frozen : TRUE; }
sp_File *sp_io_freeze(sp_File *f) { if (f) f->frozen = 1; return f; }

/* Declared in sp_sched.h, which sp_io.c does not include (it is deliberately
   self-contained); the single-threaded build gets the no-op definition. */
void sp_sched_ev_forget(int fd);

/* The close itself, once the handle is known not to be a standard stream. */
static int sp_io_close_handle(sp_File *f) {
  /* made now, so the release below never has to open it */
  (void)sp_io_closed_sentinel();
  /* Two threads closing one handle at once both passed the caller's check
     and both fclosed the FILE: a double free (two pumps of a connection
     closing it from their ensures, #8205's test). The one that sets the
     closing bit owns the close; the other finds it already taken. The owner
     takes a use of its own, so a user leaving meanwhile cannot release the
     FILE under it. */
  unsigned int u = SP_ATOMIC_LOAD(&f->uses, __ATOMIC_RELAXED);
  do {
    if (u & SP_IO_CLOSING) return 0;
  } while (!SP_ATOMIC_CAS(&f->uses, &u, (u + 1) | SP_IO_CLOSING, 1, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
  SP_ATOMIC_STORE(&f->closed, 1, __ATOMIC_RELAXED);
  sp_sched_ev_forget(fileno(SP_ATOMIC_LOAD(&f->fp, __ATOMIC_RELAXED)));
  /* a thread still inside a read or write releases the FILE when it leaves */
  return sp_io_unuse(f);
}

sp_int sp_File_close(sp_File *f) {
  /* never fclose the shared stdout/stderr handles (sp_io_stdout/sp_io_stderr):
     closing the process's standard streams would corrupt the singleton and any
     later write through it. Closing them is a no-op. */
  /* Take the descriptor out of the scheduler's readiness set BEFORE it stops
     being ours. The registration is persistent, so this is the one place a
     handle's fd dies where we can be exact; everything else that could strand
     one -- a raw close behind our back, exec, a dup we did not make -- is left
     to the self-healing arm, which re-adds on ENOENT (#4306). */
  /* The handle reads as closed BEFORE the parked readers are woken and the
     descriptor is closed, and what it carries once the FILE is released is
     the /dev/null sentinel, not the freed FILE: a reader readied by the
     forget retries on another worker and raises on the flag; one that loads
     the stream later reads EOF from the sentinel instead of dereferencing
     NULL. One inside a descriptor read holds a use, so the FILE and the
     number stay its own until it leaves (#4546). */
  FILE *fp = f ? SP_ATOMIC_LOAD(&f->fp, __ATOMIC_RELAXED) : NULL;
  if (fp && !SP_ATOMIC_LOAD(&f->closed, __ATOMIC_RELAXED) && fp != stdout && fp != stderr && fp != stdin)
    return sp_io_close_handle(f);
  if (fp && !SP_ATOMIC_LOAD(&f->closed, __ATOMIC_RELAXED)) sp_sched_ev_forget(fileno(fp));
  return 0;
}

sp_bool sp_File_closed_p(sp_File *f) {
  return !f || !SP_ATOMIC_LOAD(&f->fp, __ATOMIC_RELAXED) || SP_ATOMIC_LOAD(&f->closed, __ATOMIC_RELAXED);
}
FILE *sp_io_closed_sentinel(void) {
  static FILE *sentinel = NULL;
  /* acquire: a thread that finds the stream another one opened reads it whole */
  if (!SP_ATOMIC_LOAD(&sentinel, __ATOMIC_ACQUIRE)) {
    FILE *s = fopen("/dev/null", "r+");
    if (!s) s = fopen("/dev/null", "r");
#ifdef __wasi__
    /* WASI has no /dev/null (nor any path the host did not grant): an
       in-memory stream positioned at its end reads EOF the same way ("r"
       makes the whole buffer readable, so it starts at the end). A write
       racing a close fails here rather than being discarded. */
    if (!s) { static char none[1]; if ((s = fmemopen(none, sizeof none, "r"))) fseek(s, 0, SEEK_END); }
#endif
    if (!s) sp_raise_cls("IOError", "cannot open /dev/null");
#if !defined(_WIN32) && !defined(__wasi__)
    fcntl(fileno(s), F_SETFD, fcntl(fileno(s), F_GETFD) | FD_CLOEXEC);
#endif
    if (!SP_ATOMIC_CAS(&sentinel, &(FILE *){NULL}, s, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) fclose(s);
  }
  return SP_ATOMIC_LOAD(&sentinel, __ATOMIC_ACQUIRE);
}

/* The Ruby class a handle presents as, for the NoMethodError texts and #inspect.
   The kind label a socket carries in ->mode is what tells the socket classes
   apart; a path-backed handle is a File and everything else is an IO. */
const char *sp_io_kind_name(sp_File *f) {
  /* a NULL handle IS nil (the readiness family answers nil on timeout), so it
     names NilClass -- both for #class and for the NoMethodError texts */
  if (!f) return SPL("NilClass");
  if (f->is_sock && f->mode) {
    if (strcmp(f->mode, "tcpserver") == 0) return SPL("TCPServer");
    if (strcmp(f->mode, "tcp") == 0) return SPL("TCPSocket");
    if (strcmp(f->mode, "udp") == 0) return SPL("UDPSocket");
    if (strcmp(f->mode, "unix") == 0) return SPL("UNIXSocket");
    if (strcmp(f->mode, "unixserver") == 0) return SPL("UNIXServer");
    if (strcmp(f->mode, "socket") == 0) return SPL("Socket");
  }
  if (f->path && f->path[0] && f->path[0] != '<') return SPL("File");
  return SPL("IO");
}

/* The builtin superclass chain for a handle's class, mirroring the one the
   generated TU carries for class VALUES (sp_builtin_superclass). A handle
   answers #is_a? from its kind, which is a runtime property, so the walk lives
   here rather than in the emitted switch. io_super_name (src/analyze_util.c)
   is the compiler's copy; keep the two the same. */
static const char *sp_io_super_of(const char *k) {
  if (strcmp(k, "TCPServer") == 0)   return SPL("TCPSocket");
  if (strcmp(k, "TCPSocket") == 0)   return SPL("IPSocket");
  if (strcmp(k, "UDPSocket") == 0)   return SPL("IPSocket");
  if (strcmp(k, "IPSocket") == 0)    return SPL("BasicSocket");
  if (strcmp(k, "UNIXServer") == 0)  return SPL("UNIXSocket");
  if (strcmp(k, "UNIXSocket") == 0)  return SPL("BasicSocket");
  if (strcmp(k, "Socket") == 0)      return SPL("BasicSocket");
  if (strcmp(k, "BasicSocket") == 0) return SPL("IO");
  if (strcmp(k, "File") == 0)        return SPL("IO");
  if (strcmp(k, "IO") == 0)          return SPL("Object");
  if (strcmp(k, "Object") == 0)      return SPL("BasicObject");
  return NULL;
}
sp_bool sp_io_is_a(sp_File *f, const char *cls) {SP_GC_ROOT(f);
  if (!cls) return 0;
  if (strcmp(cls, "Kernel") == 0) return 1;   /* Object includes Kernel */
  for (const char *k = sp_io_kind_name(f); k; k = sp_io_super_of(k))
    if (strcmp(k, cls) == 0) return 1;
  return 0;
}
int sp_io_pick_class(sp_File *f, const char *const *names, const int *idx) {SP_GC_ROOT(f);
  if (!f) return 0x7fffffff;
  if (f->mode && (strcmp(f->mode, "stat") == 0 || strcmp(f->mode, "lstat") == 0)) return 0x7fffffff;
  for (const char *k = sp_io_kind_name(f); k; k = sp_io_super_of(k))
    for (int i = 0; names[i]; i++)
      if (strcmp(k, names[i]) == 0) return idx[i];
  return 0x7fffffff;
}
sp_bool sp_io_instance_of(sp_File *f, const char *cls) {SP_GC_ROOT(f);
  return cls && strcmp(sp_io_kind_name(f), cls) == 0;
}

/* Socket:: numeric constants. Their values differ between Linux and the BSDs
   (SO_REUSEADDR is 2 on Linux, 0x0004 on macOS), so they are resolved here --
   where the system headers are in scope -- rather than baked into the emitted
   C as literals. Unknown names answer -1; the caller raises NameError. */
sp_int sp_sock_const(const char *n) {
  if (!n) return -1;
  struct { const char *n; sp_int v; } T[] = {
    { "SOL_SOCKET", SOL_SOCKET }, { "IPPROTO_TCP", IPPROTO_TCP },
    { "IPPROTO_IP", IPPROTO_IP }, { "IPPROTO_UDP", IPPROTO_UDP },
    { "SO_REUSEADDR", SO_REUSEADDR }, { "SO_KEEPALIVE", SO_KEEPALIVE },
    { "SO_BROADCAST", SO_BROADCAST }, { "SO_RCVBUF", SO_RCVBUF },
    { "SO_SNDBUF", SO_SNDBUF }, { "SO_ERROR", SO_ERROR },
    { "SO_LINGER", SO_LINGER }, { "SO_TYPE", SO_TYPE },
#ifdef SO_REUSEPORT
    { "SO_REUSEPORT", SO_REUSEPORT },
#endif
    { "TCP_NODELAY", TCP_NODELAY },
    /* keepalive tuning and the timeout/credential options a client needs to
       configure a long-lived connection (#3541). Each is guarded: the names
       differ across platforms (macOS spells the idle timer TCP_KEEPALIVE). */
#ifdef TCP_KEEPIDLE
    { "TCP_KEEPIDLE", TCP_KEEPIDLE },
#elif defined(TCP_KEEPALIVE)
    { "TCP_KEEPIDLE", TCP_KEEPALIVE },
#endif
#ifdef TCP_KEEPALIVE
    { "TCP_KEEPALIVE", TCP_KEEPALIVE },
#endif
#ifdef TCP_KEEPINTVL
    { "TCP_KEEPINTVL", TCP_KEEPINTVL },
#endif
#ifdef TCP_KEEPCNT
    { "TCP_KEEPCNT", TCP_KEEPCNT },
#endif
#ifdef TCP_MAXSEG
    { "TCP_MAXSEG", TCP_MAXSEG },
#endif
#ifdef SO_RCVTIMEO
    { "SO_RCVTIMEO", SO_RCVTIMEO },
#endif
#ifdef SO_SNDTIMEO
    { "SO_SNDTIMEO", SO_SNDTIMEO },
#endif
#ifdef SO_RCVLOWAT
    { "SO_RCVLOWAT", SO_RCVLOWAT },
#endif
#ifdef SO_SNDLOWAT
    { "SO_SNDLOWAT", SO_SNDLOWAT },
#endif
#ifdef SO_OOBINLINE
    { "SO_OOBINLINE", SO_OOBINLINE },
#endif
#ifdef SO_DONTROUTE
    { "SO_DONTROUTE", SO_DONTROUTE },
#endif
#ifdef SO_ACCEPTCONN
    { "SO_ACCEPTCONN", SO_ACCEPTCONN },
#endif
#ifdef IPPROTO_IPV6
    { "IPPROTO_IPV6", IPPROTO_IPV6 },
#endif
#ifdef IPV6_V6ONLY
    { "IPV6_V6ONLY", IPV6_V6ONLY },
#endif
#ifdef IP_TTL
    { "IP_TTL", IP_TTL },
#endif
#ifdef SOCK_RAW
    { "SOCK_RAW", SOCK_RAW },
#endif
#ifdef SOCK_SEQPACKET
    { "SOCK_SEQPACKET", SOCK_SEQPACKET },
#endif
#ifdef AF_UNSPEC
    { "AF_UNSPEC", AF_UNSPEC }, { "PF_UNSPEC", PF_UNSPEC },
#endif
#ifdef MSG_PEEK
    { "MSG_PEEK", MSG_PEEK },
#endif
#ifdef MSG_WAITALL
    { "MSG_WAITALL", MSG_WAITALL },
#endif
#ifdef MSG_DONTROUTE
    { "MSG_DONTROUTE", MSG_DONTROUTE },
#endif
#ifdef MSG_OOB
    { "MSG_OOB", MSG_OOB },
#endif
    { "AF_INET", AF_INET }, { "AF_INET6", AF_INET6 }, { "AF_UNIX", AF_UNIX },
    { "PF_INET", PF_INET }, { "PF_INET6", PF_INET6 }, { "PF_UNIX", PF_UNIX },
    { "SOCK_STREAM", SOCK_STREAM }, { "SOCK_DGRAM", SOCK_DGRAM },
    { "SHUT_RD", SHUT_RD }, { "SHUT_WR", SHUT_WR }, { "SHUT_RDWR", SHUT_RDWR },
    { NULL, 0 }
  };
  for (int i = 0; T[i].n; i++) if (strcmp(T[i].n, n) == 0) return T[i].v;
  return -1;
}

/* UDPSocket.new / UNIXSocket.new / UNIXServer.new -- each wraps its fd in the
   same handle every other stream uses; the kind label is what tells them
   apart. A UDP handle keeps its own datagram path, so it is not fdopen'd. */
sp_File *sp_sock_udp_new(sp_int family) {
  extern int sp_net_udp_open(int family);
  int fd = sp_net_udp_open((int)family);
  if (fd < 0) sp_raise_cls("SocketError", "cannot create UDP socket");
  sp_File *f = (sp_File *)sp_gc_alloc(sizeof(sp_File), sp_File_fin, sp_File_scan);
  f->fp = fdopen(fd, "r+");
  if (!f->fp) { close(fd); sp_raise_cls("IOError", "fdopen failed"); return NULL; }
  f->path = NULL; f->mode = SPL("udp"); f->lineno = 0; f->is_sock = 1; f->rev_lookup = !sp_sock_dnrl_default;
  return f;
}
sp_File *sp_sock_unix_server(const char *path) {SP_GC_ROOT_STR(path);
  extern int sp_net_unix_listen(const char *path, int backlog);
  int fd = sp_net_unix_listen(path, 128);
  if (fd < 0) sp_file_raise_errno("bind", path ? path : "");
  sp_File *f = sp_io_fdopen_sock(fd, SPL("unixserver"));
  return f;
}
sp_File *sp_sock_unix_connect(const char *path) {SP_GC_ROOT_STR(path);
  extern int sp_net_unix_connect(const char *path);
  int fd = sp_net_unix_connect(path);
  if (fd < 0) sp_file_raise_errno("connect", path ? path : "");
  return sp_io_fdopen_sock(fd, SPL("unix"));
}

/* BasicSocket#do_not_reverse_lookup and its writer: a socket's own, so a
   File or a pipe answers NoMethodError, as CRuby does */
static void sp_sock_dnrl_check(sp_File *f, const char *name) {
  if (!f) sp_nil_recv(name);
  if (!f->is_sock)
    sp_raise_cls("NoMethodError", sp_sprintf("undefined method '%s' for an instance of %s",
                                             name, sp_io_kind_name(f)));
}
sp_bool sp_sock_dnrl(sp_File *f) {
  sp_sock_dnrl_check(f, "do_not_reverse_lookup");
  return !f->rev_lookup;
}
void sp_sock_set_dnrl(sp_File *f, sp_bool on) {
  sp_sock_dnrl_check(f, "do_not_reverse_lookup=");
  f->rev_lookup = !on;
}

const char *sp_sock_gethostname(void) {
  extern int sp_net_gethostname(char *buf, int cap);
  char buf[256];
  if (sp_net_gethostname(buf, (int)sizeof buf) != 0) sp_file_raise_errno("gethostname", "");
  return sp_str_from_bytes(buf, strlen(buf));
}
/* Socket.new(domain, type, protocol) -- a bare socket the bind/listen/connect
   methods then drive. */
sp_File *sp_sock_new(sp_int domain, sp_int type, sp_int proto) {
  extern int sp_net_socket(int domain, int type, int protocol);
  int fd = sp_net_socket((int)domain, (int)type, (int)proto);
  if (fd < 0) sp_file_raise_errno("socket", "");
  sp_File *f = (sp_File *)sp_gc_alloc(sizeof(sp_File), sp_File_fin, sp_File_scan);
  f->fp = fdopen(fd, "r+");
  if (!f->fp) { close(fd); sp_raise_cls("IOError", "fdopen failed"); return NULL; }
  /* Socket.new answers Socket, not the protocol-specific subclass CRuby uses
     for TCPSocket.new -- the caller asked for the generic class. */
  f->path = NULL; f->mode = SPL("socket"); f->lineno = 0; f->is_sock = 1; f->rev_lookup = !sp_sock_dnrl_default;
  return f;
}
/* An Integer argument a C int parameter takes, checked in its full width
   first: a wider value cut down to an int could name another port or open
   descriptor. Worded as CRuby's NUM2INT. */
static int sp_sock_int_arg(sp_int v) {
  if (v > INT_MAX) sp_raise_cls("RangeError", sp_sprintf("integer %lld too big to convert to 'int'", (long long)v));
  if (v < INT_MIN) sp_raise_cls("RangeError", sp_sprintf("integer %lld too small to convert to 'int'", (long long)v));
  return (int)v;
}
/* Socket.tcp_server_sockets(host, port) -> [Socket, ...]: a listening Socket
   per address the host resolves to (an IPv4 and an IPv6 one for a nil host on
   a dual-stack machine). Raises what the last address failed with when none
   could be bound, as CRuby does. */
sp_PolyArray *sp_sock_tcp_server_sockets(const char *host, sp_int port) {SP_GC_ROOT_STR(host);
  extern int sp_net_listen_all(const char *host, int port, int *fds, int cap, const char **op);
  int fds[16];
  const char *op = NULL;
  int n = sp_net_listen_all(host, sp_sock_int_arg(port), fds, 16, &op);
  if (n < 0) sp_raise_cls("SocketError", sp_sprintf("getaddrinfo: %s", op));
  if (n == 0) {
    int e = errno;
    sp_raise_cls(sp_errno_class_name(e),
                 sp_sprintf("%s - %s for %s:%d", strerror(e), op,
                            (host && *host) ? host : "nil", (int)port));
  }
  sp_PolyArray *a = sp_PolyArray_new();
  SP_GC_ROOT(a);
  for (int i = 0; i < n; i++)
    sp_PolyArray_push(a, sp_box_obj(sp_io_fdopen_sock(fds[i], SPL("socket")), SP_BUILTIN_IO));
  return a;
}
/* TCPServer.for_fd(fd) and the other socket classes': wrap a descriptor the
   program already holds as that class. The handle owns the fd from then on,
   so the usual move is to set autoclose = false on the one it came from. */
sp_File *sp_sock_for_fd(sp_int fd, sp_int kind) {
  int d = sp_sock_int_arg(fd);
  if (d < 0 || fcntl(d, F_GETFD) < 0)
    sp_raise_cls("Errno::EBADF", "Bad file descriptor");
  const char *k = kind == 1 ? SPL("tcpserver") : kind == 2 ? SPL("tcp") :
                  kind == 3 ? SPL("udp") : kind == 4 ? SPL("unixserver") :
                  kind == 5 ? SPL("unix") : SPL("socket");
  return sp_io_fdopen_sock(d, k);
}
/* Socket.pair / Socket.socketpair -> the two connected ends. */
sp_File *sp_sock_pair_end(sp_int domain, sp_int type, sp_int proto, sp_int which) {
  extern int sp_net_socketpair(int domain, int type, int protocol, int fds[2]);
  static int cached[2] = { -1, -1 };
  if (which == 0) {
    if (sp_net_socketpair((int)domain, (int)type, (int)proto, cached) != 0)
      sp_file_raise_errno("socketpair", "");
  }
  int fd = cached[which == 0 ? 0 : 1];
  if (fd < 0) sp_raise_cls("IOError", "socketpair");
  (void)domain; (void)type;
  return sp_io_fdopen_sock(fd, SPL("socket"));
}

/* Socket.getaddrinfo: one row per resolution, in CRuby's 7-element shape. */
/* A family or socktype argument of Socket.getaddrinfo: nil is 0
   (unrestricted), an Integer as given, a String or Symbol by name with or
   without its AF_ / SOCK_ prefix, as CRuby reads it. An unknown name is CRuby's
   SocketError. */
sp_int sp_sock_addrinfo_hint(sp_RbVal v, sp_int is_family) {
  v = sp_rbval_str(v);
  if (v.tag == SP_TAG_NIL) return 0;
  if (v.tag == SP_TAG_INT) return v.v.i;
  const char *name = NULL;
  if (v.tag == SP_TAG_STR) name = v.v.s;
  else if (v.tag == SP_TAG_SYM && sp_sym_name_fn) name = sp_sym_name_fn((sp_sym)v.v.i);
  if (!name) sp_raise_cls("TypeError", "no implicit conversion into Integer");
  const char *prefix = is_family ? "AF_" : "SOCK_";
  size_t pl = strlen(prefix);
  if (strncmp(name, prefix, pl) == 0) name += pl;
  if (is_family && strcmp(name, "UNSPEC") == 0) return 0;
  char full[64];
  snprintf(full, sizeof full, "%s%s", prefix, name);
  sp_int r = sp_sock_const(full);
  if (r < 0) sp_raise_cls("SocketError", is_family ? "unknown socket domain" : "unknown socket type");
  return r;
}

/* Socket.getaddrinfo(host, port [, family [, socktype]]): a family or socktype
   of 0 (nil) leaves it unrestricted, as in CRuby. */
sp_PolyArray *sp_sock_getaddrinfo(const char *host, sp_int port, sp_int family, sp_int socktype) {SP_GC_ROOT_STR(host);
  extern int sp_net_getaddrinfo_at(const char *host, int port, int want_family, int socktype, int idx,
                                   int *family, int *stype, int *proto,
                                   char *ipbuf, int ipcap, int *port_out);
  sp_PolyArray *out = sp_PolyArray_new();
  SP_GC_ROOT(out);
  for (int i = 0; i < 64; i++) {
    int fam = 0, stype = 0, proto = 0, p = 0;
    char ip[64];
    if (sp_net_getaddrinfo_at(host, (int)port, (int)family, (int)socktype, i, &fam, &stype, &proto,
                              ip, (int)sizeof ip, &p) != 0) break;
    const char *ips = sp_str_from_bytes(ip, strlen(ip));
    SP_GC_ROOT_STR(ips);   /* held across the row's allocation */
    sp_PolyArray *row = sp_PolyArray_new();
    sp_PolyArray_push(row, sp_box_str(fam == AF_INET6 ? SPL("AF_INET6") : SPL("AF_INET")));
    sp_PolyArray_push(row, sp_box_int((sp_int)p));
    sp_PolyArray_push(row, sp_box_str(ips));
    sp_PolyArray_push(row, sp_box_str(ips));
    sp_PolyArray_push(row, sp_box_int((sp_int)fam));
    sp_PolyArray_push(row, sp_box_int((sp_int)stype));
    sp_PolyArray_push(row, sp_box_int((sp_int)proto));
    sp_PolyArray_push(out, sp_box_poly_array(row));
  }
  return out;
}

/* IPSocket.getaddress(host): the first address the name resolves to, as
   CRuby answers it; SocketError with getaddrinfo's message when none */
const char *sp_sock_getaddress(const char *host) {SP_GC_ROOT_STR(host);
  extern int sp_net_getaddress(const char *host, char *ipbuf, int ipcap, const char **err);
  char ip[64];
  const char *err = NULL;
  if (sp_net_getaddress(host, ip, (int)sizeof ip, &err) != 0)
    sp_raise_cls("SocketError", sp_sprintf("getaddrinfo: %s", err ? err : ""));
  return sp_str_from_bytes(ip, strlen(ip));
}

/* Socket.sockaddr_in(port, host) / Socket.pack_sockaddr_in: the packed
   sockaddr, as a byte string. It carries NUL bytes (a zero port octet, the
   sin_zero padding, an address with a zero octet), so it is built with
   sp_str_from_bytes rather than a C-string constructor -- the string this
   returns is the one connect_nonblock(sockaddr) and bind take. */
const char *sp_sock_pack_sockaddr_in(sp_int port, const char *host) {SP_GC_ROOT_STR(host);
  extern int sp_net_pack_sockaddr_in(const char *host, int port, void *out, int cap);
  char buf[128];
  int n = sp_net_pack_sockaddr_in(host, (int)port, buf, (int)sizeof buf);
  if (n < 0)
    sp_raise_cls("SocketError",
                 sp_sprintf("getaddrinfo: Name or service not known - %s", host ? host : ""));
  return sp_str_from_bytes(buf, (size_t)n);
}

const char *sp_sock_pack_sockaddr_un(const char *path) {SP_GC_ROOT_STR(path);
  extern int sp_net_pack_sockaddr_un(const char *path, void *out, int cap);
  char buf[128 + 108];
  int n = sp_net_pack_sockaddr_un(path, buf, (int)sizeof buf);
  if (n < 0)
    sp_raise_cls("ArgumentError",
                 sp_sprintf("too long unix socket path (%d bytes given)",
                            path ? (int)strlen(path) : 0));
  return sp_str_from_bytes(buf, (size_t)n);
}

/* Addrinfo#to_sockaddr / #to_s -- the same packed form for an endpoint that is
   already resolved, which is the whole reason the 1-arg connect exists: the
   caller does not pay for the name lookup twice. AF_UNIX goes through the path
   form; everything else is already numeric, so the getaddrinfo below is a
   parse rather than a resolution. */
const char *sp_addrinfo_to_sockaddr(sp_Addrinfo *a) {SP_GC_ROOT(a);
  if (!a) sp_raise_cls("SocketError", "no address");
  if (a->afname && strcmp(a->afname, "AF_UNIX") == 0)
    return sp_sock_pack_sockaddr_un(a->ip);
  return sp_sock_pack_sockaddr_in(a->port, a->ip);
}

/* Socket.unpack_sockaddr_in -> [port, host]. The inverse, and what makes a
   packed address inspectable from Ruby without byte hacks. */
sp_PolyArray *sp_sock_unpack_sockaddr_in(const char *sa) {SP_GC_ROOT_STR(sa);
  extern int sp_net_unpack_sockaddr_in(const void *sa, int salen, char *ipbuf, int cap);
  char ip[128];
  int len = sa ? (int)sp_str_byte_len(sa) : 0;
  int port = sp_net_unpack_sockaddr_in(sa, len, ip, (int)sizeof ip);
  if (port < 0) sp_raise_cls("ArgumentError", "not an AF_INET/AF_INET6 sockaddr");
  sp_PolyArray *out = sp_PolyArray_new();
  SP_GC_ROOT(out);
  sp_PolyArray_push(out, sp_box_int((sp_int)port));
  sp_PolyArray_push(out, sp_box_str(sp_str_from_bytes(ip, strlen(ip))));
  return out;
}

/* Socket.unpack_sockaddr_un -> the path, the inverse of sockaddr_un. */
const char *sp_sock_unpack_sockaddr_un(const char *sa) {SP_GC_ROOT_STR(sa);
  extern int sp_net_unpack_sockaddr_un(const void *sa, int salen, char *pathbuf, int cap);
  char path[256];
  int len = sa ? (int)sp_str_byte_len(sa) : 0;
  int n = sp_net_unpack_sockaddr_un(sa, len, path, (int)sizeof path);
  if (n == -2)
    sp_raise_cls("TypeError", sp_sprintf("too long sockaddr_un - %d longer than %d",
                                         len, (int)sizeof(struct sockaddr_un)));
  if (n < 0) sp_raise_cls("ArgumentError", "not an AF_UNIX sockaddr");
  return sp_str_from_bytes(path, (size_t)n);
}

/* #local_address / #remote_address -> Addrinfo for this end / the peer. */
sp_Addrinfo *sp_sock_address(sp_File *f, sp_int peer) {SP_GC_ROOT(f);
  extern sp_Addrinfo *sp_addrinfo_new(const char *ip, sp_int port, sp_int stype, sp_int is_unix);
  extern int sp_net_sock_ip(int fd, int peer, char *ipbuf, int cap);
  extern int sp_net_unix_path(int fd, int peer, char *buf, int cap);
  if (!f || !f->is_sock)
    sp_raise_cls("NoMethodError",
                 sp_sprintf("undefined method '%s' for an instance of %s",
                            peer ? "remote_address" : "local_address", sp_io_kind_name(f)));
  SP_IO_OPEN(f);
  int fd = fileno(f->fp);
  const char *k = sp_io_kind_name(f);
  int is_unix = (strcmp(k, "UNIXSocket") == 0 || strcmp(k, "UNIXServer") == 0);
  /* The address goes to sp_addrinfo_new as a String: it roots its argument
     across its allocation, and a root on this stack buffer had the
     collector read the byte before the array for a tag. */
  char buf[256];
  buf[0] = '\0';
  if (is_unix) {
    sp_net_unix_path(fd, (int)peer, buf, (int)sizeof buf);
    return sp_addrinfo_new(sp_str_dup_external(buf), 0, SOCK_STREAM, 1);
  }
  int port = sp_net_sock_ip(fd, (int)peer, buf, (int)sizeof buf);
  if (port < 0) { buf[0] = '\0'; port = 0; }
  int stype = (strcmp(k, "UDPSocket") == 0) ? SOCK_DGRAM : SOCK_STREAM;
  return sp_addrinfo_new(sp_str_dup_external(buf), (sp_int)port, stype, 0);
}

/* #getsockname / #getpeername -> the packed sockaddr of this end / the peer,
   as a binary String (what Socket.unpack_sockaddr_in and Addrinfo take). */
const char *sp_sock_getname(sp_File *f, sp_int peer) {SP_GC_ROOT(f);
  extern int sp_net_sock_name(int fd, int peer, void *out, int cap);
  const char *m = peer ? "getpeername" : "getsockname";
  if (!f || !f->is_sock)
    sp_raise_cls("NoMethodError",
                 sp_sprintf("undefined method '%s' for an instance of %s", m, sp_io_kind_name(f)));
  SP_IO_OPEN(f);
  char buf[256];
  int n = sp_net_sock_name(fileno(f->fp), (int)peer, buf, (int)sizeof buf);
  if (n < 0) {
    int e = errno;
    sp_raise_cls(sp_errno_class_name(e), sp_sprintf("%s - %s(2)", strerror(e), m));
  }
  char *s = (char *)sp_str_from_bytes(buf, (size_t)n);
  sp_str_mark_binary(s);
  return s;
}

/* Only a socket answers the socket-specific methods; say which class the
   receiver actually is when it does not. */
static void sp_sock_require(sp_File *f, const char *m) {SP_GC_ROOT(f);SP_GC_ROOT_STR(m);
  if (!f || !f->is_sock)
    sp_raise_cls("NoMethodError",
                 sp_sprintf("undefined method '%s' for an instance of %s", m, sp_io_kind_name(f)));
  SP_IO_OPEN(f);
}
sp_int sp_sock_bind(sp_File *f, const char *host, sp_int port) {SP_GC_ROOT(f);SP_GC_ROOT_STR(host);
  extern int sp_net_udp_bind(int fd, const char *host, int port);
  sp_sock_require(f, "bind");
  if (sp_net_udp_bind(fileno(f->fp), host, (int)port) != 0)
    sp_file_raise_errno("bind", host ? host : "");
  return 0;
}
sp_int sp_sock_connect(sp_File *f, const char *host, sp_int port) {SP_GC_ROOT(f);SP_GC_ROOT_STR(host);
  extern int sp_net_udp_connect(int fd, const char *host, int port);
  sp_sock_require(f, "connect");
  if (sp_net_udp_connect(fileno(f->fp), host, (int)port) != 0)
    sp_file_raise_errno("connect", host ? host : "");
  return 0;
}
sp_int sp_sock_send(sp_File *f, const char *data, sp_int len, const char *host, sp_int port) {SP_GC_ROOT(f);SP_GC_ROOT_STR(data);SP_GC_ROOT_STR(host);
  extern int sp_net_udp_send_to(int fd, const char *data, int len, const char *host, int port);
  sp_sock_require(f, "send");
  int n = sp_net_udp_send_to(fileno(f->fp), data, (int)len, host, (int)port);
  if (n < 0) sp_file_raise_errno("send", host ? host : "");
  return (sp_int)n;
}
static long sp_io_buffered(FILE *fp);
/* #recv reads one datagram (or up to `len` stream bytes) as a String;
   #recvfrom pairs it with the sender's address, CRuby's 4-element form.
   The payload is BINARY in every shape, as CRuby answers it. */
const char *sp_sock_recv(sp_File *f, sp_int len) {SP_GC_ROOT(f);
  extern int sp_net_udp_recv_from(int fd, char *buf, int cap, char *ipbuf, int ipcap, int *port_out);
  sp_sock_require(f, "recv");
  if (len < 0) sp_raise_cls("ArgumentError", "negative string size (or size too big)");
  /* bytes a read on the stream already pulled into stdio's buffer are the
     peer's next ones: served first, as readpartial does, or recv stepped
     over them (#7195) */
  long pend = sp_io_buffered(sp_io_use(f));
  sp_io_unuse(f);
  /* a zero-length read of a stream with bytes waiting answers nil, as
     recv(2) of none answers 0 below */
  if (pend > 0 && len == 0) return NULL;
  if (pend > 0) {
    size_t want = (size_t)len < (size_t)pend ? (size_t)len : (size_t)pend;
    char *r = sp_str_alloc(want);
    size_t got = SP_SIG_BLOCKING(fread(r, 1, want, sp_io_use(f)));
    sp_io_unuse(f);
    r[got] = 0;
    sp_str_set_len(r, got);
    sp_str_mark_binary(r);
    return r;
  }
  /* park until the peer writes, as readpartial does: a close from another
     thread then wakes this one with CRuby's IOError, where a recv(2) already
     blocked in the kernel answered EBADF, or read a descriptor that was by
     then someone else's (#7555) */
  /* (a zero-length read still waits for the peer, as CRuby's does: a
     datagram it then takes is discarded, and a stream answers nil) */
  sp_io_wait_readable(f);
  char *buf = (char *)malloc(len > 0 ? (size_t)len : 1);
  if (!buf) sp_raise_cls("NoMemoryError", "recv");
  FILE *fp = sp_io_try_use(f);
  if (!fp) { free(buf); sp_raise_cls("IOError", "stream closed in another thread"); }
  int fd = fileno(fp);
  int n = sp_net_udp_recv_from(fd, buf, (int)len, NULL, 0, NULL);
  int e = errno;
  /* a stream socket at EOF answers nil (Ruby 3.3 and later); an empty
     datagram is still "" */
  int st = 0; socklen_t sl = sizeof st;
  int eof = n == 0 && getsockopt(fd, SOL_SOCKET, SO_TYPE, &st, &sl) == 0 && st == SOCK_STREAM;
  sp_io_unuse(f);
  errno = e;
  if (n < 0) { free(buf); sp_file_raise_errno("recv", ""); }
  if (eof) { free(buf); return NULL; }
  char *s = (char *)sp_str_from_bytes(buf, (size_t)n);
  free(buf);
  sp_str_mark_binary(s);
  return s;
}
/* Fills the caller's address slots; returns the payload. */
const char *sp_sock_recvfrom(sp_File *f, sp_int len, const char **ip_out, sp_int *port_out) {SP_GC_ROOT(f);
  extern int sp_net_udp_recv_from(int fd, char *buf, int cap, char *ipbuf, int ipcap, int *port_out);
  sp_sock_require(f, "recvfrom");
  char ipbuf[64];
  int port = 0;
  if (len < 0) sp_raise_cls("ArgumentError", "negative string size (or size too big)");
  /* (a zero-length read waits and takes a datagram too, as sp_sock_recv) */
  sp_io_wait_readable(f);   /* as sp_sock_recv (#7555) */
  char *buf = (char *)malloc(len > 0 ? (size_t)len : 1);
  if (!buf) sp_raise_cls("NoMemoryError", "recvfrom");
  FILE *fp = sp_io_try_use(f);
  if (!fp) { free(buf); sp_raise_cls("IOError", "stream closed in another thread"); }
  int n = sp_net_udp_recv_from(fileno(fp), buf, (int)len, ipbuf, (int)sizeof ipbuf, &port);
  { int e = errno; sp_io_unuse(f); errno = e; }
  if (n < 0) { free(buf); sp_file_raise_errno("recvfrom", ""); }
  char *s = (char *)sp_str_from_bytes(buf, (size_t)n); SP_GC_ROOT_STR(s);   /* across the address's copy */
  free(buf);
  sp_str_mark_binary(s);
  *ip_out = sp_str_from_bytes(ipbuf, strlen(ipbuf));
  *port_out = (sp_int)port;
  return s;
}
sp_int sp_sock_shutdown(sp_File *f, sp_int how) {SP_GC_ROOT(f);
  extern int sp_net_shutdown(int fd, int how);
  sp_sock_require(f, "shutdown");
  if (sp_net_shutdown(fileno(f->fp), (int)how) != 0) sp_file_raise_errno("shutdown", "");
  return 0;
}
/* A setsockopt value as the int the option takes: an Integer, true/false as
   1/0, or the option's packed bytes ([1].pack("i")), as CRuby accepts it. */
sp_int sp_sock_optval(sp_RbVal v) {
  /* a shared String handle reads as its bytes (sp_String's first field) */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_STRBUF && v.v.p) { v.tag = SP_TAG_STR; v.v.s = *(const char **)v.v.p; }
  if (v.tag == SP_TAG_INT) return v.v.i;
  if (v.tag == SP_TAG_BOOL) return v.v.i != 0;
  if (v.tag == SP_TAG_STR && v.v.s) {
    int n = 0;
    if (sp_str_byte_len(v.v.s) != sizeof n)
      sp_raise_cls("ArgumentError", "only an int-sized packed option value is supported");
    memcpy(&n, v.v.s, sizeof n);
    return n;
  }
  sp_raise_cls("TypeError", "no implicit conversion into Integer");
  return 0;
}

sp_int sp_sock_setsockopt(sp_File *f, sp_int level, sp_int opt, sp_int value) {SP_GC_ROOT(f);
  extern int sp_net_setsockopt_int(int fd, int level, int optname, int value);
  sp_sock_require(f, "setsockopt");
  if (sp_net_setsockopt_int(fileno(f->fp), (int)level, (int)opt, (int)value) != 0)
    sp_file_raise_errno("setsockopt", "");
  return 0;
}
sp_SockOpt *sp_sock_getsockopt(sp_File *f, sp_int level, sp_int opt) {SP_GC_ROOT(f);
  extern int sp_net_getsockopt_int(int fd, int level, int optname);
  extern sp_SockOpt *sp_sockopt_new(sp_int family, sp_int level, sp_int optname, sp_int value);
  extern int sp_net_fd_family(int fd);
  sp_sock_require(f, "getsockopt");
  int v = sp_net_getsockopt_int(fileno(f->fp), (int)level, (int)opt);
  return sp_sockopt_new((sp_int)sp_net_fd_family(fileno(f->fp)), level, opt, (sp_int)v);
}
sp_int sp_sock_listen(sp_File *f, sp_int backlog) {SP_GC_ROOT(f);
  sp_sock_require(f, "listen");
  if (listen(fileno(f->fp), (int)backlog) != 0) sp_file_raise_errno("listen", "");
  return 0;
}

/* ---- the non-blocking readiness family ----
   Each call tries once on a non-blocking descriptor. Would-block is not an
   error: with `exception: false` the caller gets :wait_readable /
   :wait_writable, otherwise the matching IO::*Wait* exception, which carries
   both its Errno parent and the IO::Wait* module (see lib/sp_exc.c). */
/* Bytes stdio already buffered for this handle. A #gets followed by a
   #read_nonblock must see them: the raw read(2) below goes straight to the
   descriptor and would skip the buffer. Same platform probe sp_io_wait_readable
   uses. The stream is one the caller holds a use of (sp_io_use). */
static long sp_io_buffered(FILE *fp) {
#if defined(__GLIBC__)
  return (long)(fp->_IO_read_end - fp->_IO_read_ptr);
#elif defined(__APPLE__)
  return (long)fp->_r;
#else
  return (long)__freadahead(fp);
#endif
}
/* O_NONBLOCK is set for the duration of one call and put back: leaving it on
   would make a later blocking #gets treat EAGAIN as EOF. */
static int sp_io_nb_begin(sp_File *f) {
  int fd = fileno(f->fp);
  int fl = fcntl(fd, F_GETFL);
  if (fl >= 0 && !(fl & O_NONBLOCK)) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  return fl;
}
static void sp_io_nb_end(sp_File *f, int saved) {
  if (saved >= 0 && !(saved & O_NONBLOCK)) fcntl(fileno(f->fp), F_SETFL, saved);
}

static ssize_t sp_io_read_chunk_timed(sp_File *f, char *buf, size_t count) {
  SP_GC_ROOT(f);
  double deadline = -2.0;
  for (;;) {
    FILE *fp = sp_io_use(f);
    size_t buffered = sp_io_stdio_buffered(fp);
    if (buffered) {
      size_t want = buffered < count ? buffered : count;
      size_t got = SP_SIG_BLOCKING(fread(buf, 1, want, fp));
      sp_io_unuse(f);
      return (ssize_t)got;
    }
    sp_io_unuse(f);
    if (!sp_io_parkable(f)) {
      (void)sp_io_timeout_seconds(f);
      fp = sp_io_use(f);
      size_t got = SP_SIG_BLOCKING(fread(buf, 1, count, fp));
      sp_io_unuse(f);
      return (ssize_t)got;
    }
    if (deadline == -2.0) deadline = sp_io_deadline(f);
    sp_io_wait_deadline(f, POLLIN, deadline);
    fp = sp_io_use(f);
    int saved = sp_io_nb_begin(f);
    ssize_t got = SP_SIG_BLOCKING(read(fileno(fp), buf, count));
    int error = errno;
    sp_io_nb_end(f, saved);
    sp_io_unuse(f);
    if (got >= 0) return got;
    if (error == EINTR || error == EAGAIN || error == EWOULDBLOCK) continue;
    errno = error;
    sp_file_raise_errno("read", "");
  }
}

const char *sp_File_read_timed(sp_File *f, sp_int n) {
  SP_GC_ROOT(f);
  SP_IO_OPEN(f);
  if (n == 0) return sp_str_empty_binary();
  size_t cap = n < 0 ? 8192 : (size_t)n;
  size_t len = 0;
  char *result = sp_str_alloc(cap);
  SP_GC_ROOT_STR(result);
  for (;;) {
    if (len == cap) {
      if (n >= 0) break;
      if (cap > SIZE_MAX / 2) sp_oom_die();
      char *grown = sp_str_alloc(cap * 2);
      memcpy(grown, result, len);
      result = grown;
      cap *= 2;
    }
    ssize_t got = sp_io_read_chunk_timed(f, result + len, cap - len);
    if (got == 0) break;
    len += (size_t)got;
  }
  if (n > 0 && len == 0) return NULL;
  result[len] = 0;
  sp_str_set_len(result, len);
  if (n >= 0 || sp_File_reads_binary(f)) sp_str_mark_binary(result);
  return result;
}

static void sp_sock_nb_prepare(sp_File *f, const char *m) {SP_GC_ROOT(f);SP_GC_ROOT_STR(m);
  if (!f || !f->is_sock)
    sp_raise_cls("NoMethodError",
                 sp_sprintf("undefined method '%s' for an instance of %s", m, sp_io_kind_name(f)));
  SP_IO_OPEN(f);
}
SP_NORETURN static void sp_sock_raise_wait(int writable, const char *op) {SP_GC_ROOT_STR(op);
  sp_raise_cls(writable ? "IO::EAGAINWaitWritable" : "IO::EAGAINWaitReadable",
               sp_sprintf("Resource temporarily unavailable - %s would block", op));
}
static int sp_sock_would_block(void) {
  return errno == EAGAIN || errno == EWOULDBLOCK;
}
/* accept_nonblock -> the new handle, or nil / the :wait_readable marker. */
sp_File *sp_sock_accept_nb(sp_File *f, sp_bool exc) {SP_GC_ROOT(f);
  extern int sp_net_accept_nb(int sfd);
  sp_sock_nb_prepare(f, "accept_nonblock");
  int saved = sp_io_nb_begin(f);
  int fd = sp_net_accept_nb(fileno(f->fp));
  int e = errno;
  sp_io_nb_end(f, saved);
  errno = e;
  if (fd >= 0) return sp_io_fdopen_sock(fd, SPL("tcp"));
  if (sp_sock_would_block()) {
    if (!exc) return NULL;
    sp_sock_raise_wait(0, "accept");
  }
  sp_file_raise_errno("accept", "");
}
/* read_nonblock / recv_nonblock -> the bytes read, or nil when it would block
   and the caller asked for no exception. EOF is nil in CRuby only for
   `exception: false`; otherwise it is EOFError. */
/* IO#readpartial / #sysread: at most n bytes, returning as soon as ANY are
   available. It used to be one fread of the full n, which is a different
   operation: fread keeps calling read(2) until it has n bytes or hits EOF. On
   a socket that blocks until the peer sends n or closes, so the read-then-write
   shape every HTTP server has -- read the request, write the response --
   deadlocks, both ends waiting for the other (#3379). Where libc returns a
   short fread instead of blocking, the same call left the stream in a state
   the following write did not survive.

   Buffer first, exactly as sp_sock_read_nb does: whatever stdio already holds
   is data the peer has sent, and a raw read(2) would step over it, so a #gets
   before a #readpartial would lose bytes. Then a single BLOCKING read -- that
   is the only difference from the nonblocking sibling below.

   Park first, like every other read entry point: the blocking read(2) sits on
   the OS worker, which never reaches a safepoint, so the next stop-the-world
   collection waited on it forever and one green thread idle in readpartial
   on a quiet socket froze every other thread's allocation (#4528). */
const char *sp_File_readpartial_or_nil(sp_File *f, sp_int n) {SP_GC_ROOT(f);
  SP_IO_OPEN(f);
  if (n < 0) sp_raise_cls("EOFError", "end of file reached");
  if (n == 0) return sp_str_empty_binary();
  sp_io_wait_readable(f);
  char *r = sp_str_alloc((size_t)n);
  ssize_t got;
  FILE *fp = sp_io_use(f);
  long pend = sp_io_buffered(fp);
  if (pend > 0) {
    size_t want = (size_t)n < (size_t)pend ? (size_t)n : (size_t)pend;
    got = (ssize_t)SP_SIG_BLOCKING(fread(r, 1, want, fp));
  }
  else {
    do { got = SP_SIG_BLOCKING(read(fileno(fp), r, (size_t)n)); } while (got < 0 && errno == EINTR);
  }
  { int e = errno; sp_io_unuse(f); errno = e; }
  if (got < 0) sp_file_raise_errno("read", "");
  if (got == 0) return NULL;
  r[got] = 0;
  sp_str_set_len(r, (size_t)got);
  sp_str_mark_binary(r);   /* readpartial / sysread answer ASCII-8BIT, as CRuby */
  return r;
}

/* readpartial / sysread: EOFError at the end of the stream. */
const char *sp_File_readpartial(sp_File *f, sp_int n) {
  const char *r = sp_File_readpartial_or_nil(f, n);
  if (!r) sp_raise_cls("EOFError", "end of file reached");
  return r;
}

const char *sp_sock_read_nb(sp_File *f, sp_int len, sp_bool exc, sp_bool is_recv, sp_bool *eof) {SP_GC_ROOT(f);
  if (eof) *eof = 0;
  if (is_recv) sp_sock_nb_prepare(f, "recv_nonblock");
  else SP_IO_OPEN(f);
  /* the bytes read are BINARY, as CRuby's read_nonblock and recv answer them */
  if (len <= 0) { char *e = (char *)sp_str_from_bytes("", 0); sp_str_mark_binary(e); return e; }
  char *buf = (char *)malloc((size_t)len);
  if (!buf) sp_raise_cls("NoMemoryError", "read_nonblock");
  ssize_t n;
  FILE *fp = sp_io_try_use(f);
  if (!fp) { free(buf); sp_raise_cls("IOError", "stream closed in another thread"); }
  long pend = sp_io_buffered(fp);
  if (pend > 0) {   /* serve what stdio already holds, as CRuby does */
    size_t want = (size_t)len < (size_t)pend ? (size_t)len : (size_t)pend;
    n = (ssize_t)SP_SIG_BLOCKING(fread(buf, 1, want, fp));
  }
  else {
    int saved = sp_io_nb_begin(f);
    do { n = SP_SIG_BLOCKING(read(fileno(fp), buf, (size_t)len)); } while (n < 0 && errno == EINTR);
    int e = errno;
    sp_io_nb_end(f, saved);
    errno = e;
  }
  { int e = errno; sp_io_unuse(f); errno = e; }
  if (n > 0) { char *s = (char *)sp_str_from_bytes(buf, (size_t)n); free(buf); sp_str_mark_binary(s); return s; }
  if (n == 0) {
    free(buf);
    if (eof) *eof = 1;
    /* recv_nonblock answers nil at EOF in BOTH forms (Ruby 3.3 and later;
       it was "" before) -- it does not raise EOFError. read_nonblock is the
       one that tells them apart: nil for `exception: false`, EOFError
       otherwise. */
    if (is_recv) return NULL;
    if (!exc) return NULL;                     /* CRuby: nil at EOF */
    sp_raise_cls("EOFError", "end of file reached");
  }
  free(buf);
  if (sp_sock_would_block()) {
    if (!exc) return NULL;
    sp_sock_raise_wait(0, "read");
  }
  sp_file_raise_errno("read", "");
}
/* write_nonblock -> the byte count, or nil when it would block.
   Paired like sp_File_write: the _bin entry sizes with the header length and
   is emitted only for a String value; this one stays strlen for bare
   literals (a poly operand reaches it through sp_poly_to_s, which answers
   static class/symbol names with no marker byte). */
static sp_oint sp_sock_write_nb_len(sp_File *f, const char *data, size_t len, sp_bool exc) {SP_GC_ROOT(f);SP_GC_ROOT_STR(data);
  ssize_t n;
  int saved = sp_io_nb_begin(f);
  do { n = write(fileno(f->fp), data ? data : "", len); } while (n < 0 && errno == EINTR);
  int we = errno;
  sp_io_nb_end(f, saved);
  errno = we;
  if (n >= 0) return sp_oint_of((sp_int)n);
  if (sp_sock_would_block()) {
    if (!exc) return sp_oint_nil();
    sp_sock_raise_wait(1, "write");
  }
  sp_file_raise_errno("write", "");
}
sp_oint sp_sock_write_nb(sp_File *f, const char *data, sp_bool exc) {SP_GC_ROOT(f);SP_GC_ROOT_STR(data);
  SP_IO_OPEN(f);
  return sp_sock_write_nb_len(f, data, data ? strlen(data) : 0, exc);
}
sp_oint sp_sock_write_nb_bin(sp_File *f, const char *data, sp_bool exc) {SP_GC_ROOT(f);SP_GC_ROOT_STR(data);
  SP_IO_OPEN(f);
  return sp_sock_write_nb_len(f, data, data ? sp_str_byte_len(data) : 0, exc);
}
/* connect_nonblock: an in-flight connect is IO::EINPROGRESSWaitWritable. */
sp_oint sp_sock_connect_nb(sp_File *f, const char *host, sp_int port, sp_bool exc) {SP_GC_ROOT(f);SP_GC_ROOT_STR(host);
  extern int sp_net_udp_connect(int fd, const char *host, int port);
  sp_sock_nb_prepare(f, "connect_nonblock");
  int saved = sp_io_nb_begin(f);
  int rc = sp_net_udp_connect(fileno(f->fp), host, (int)port);
  int ce = errno;
  sp_io_nb_end(f, saved);
  errno = ce;
  if (rc == 0) return sp_oint_of(0);
  if (errno == EINPROGRESS || errno == EALREADY) {
    if (!exc) return sp_oint_nil();
    sp_raise_cls("IO::EINPROGRESSWaitWritable", "operation in progress - connect(2) would block");
  }
  if (errno == EISCONN) return sp_oint_of(0);
  sp_file_raise_errno("connect", host ? host : "");
}
/* connect_nonblock(sockaddr). The peer is already resolved into a packed
   struct sockaddr (the binary String returned by Socket.sockaddr_in or
   #to_sockaddr on an Addrinfo). connect(2) takes the raw bytes; we just
   pass them through -- from an aligned copy. The GC String's data pointer
   carries no alignment promise, and netstacks may require one: lwIP's
   connect guard (IS_SOCK_ADDR_ALIGNED, MEM_ALIGNMENT=4) silently rejects
   an unaligned address with ERR_ARG -> EIO before any packet is sent
   (observed on ESP32: every 1-arg connect from a freshly allocated
   String failed in microseconds while bind() -- which passes the
   resolver's own aligned addrinfo buffer -- worked). Copy through a
   maximally-aligned local, sized for sockaddr_storage. Same nonblocking
   lifecycle as sp_sock_connect_nb. */
sp_oint sp_sock_connect_nb_sa(sp_File *f, const char *sa, sp_int salen, sp_bool exc) {SP_GC_ROOT(f);SP_GC_ROOT_STR(sa);
  /* sp_sock_nb_prepare runs first: it checks the closed-socket and
     wrong-class guards and raises the matching exception (e.g. IOError
     "closed stream") before we touch the address. The sa/salen guard
     comes after, sets errno = EINVAL, and routes through the same
     errno->raise path every other socket op uses. */
  sp_sock_nb_prepare(f, "connect_nonblock");
  if (!sa || salen <= 0) {
    errno = EINVAL;
    sp_file_raise_errno("connect", "");
  }
  if (salen > (sp_int)sizeof(struct sockaddr_storage)) {
    errno = EINVAL;
    sp_file_raise_errno("connect", "");
  }
  struct sockaddr_storage aligned_sa;
  memcpy(&aligned_sa, sa, (size_t)salen);
  int saved = sp_io_nb_begin(f);
  int rc = SP_SIG_BLOCKING(connect(fileno(f->fp), (const struct sockaddr *)&aligned_sa, (socklen_t)salen));
  int ce = errno;
  sp_io_nb_end(f, saved);
  errno = ce;
  if (rc == 0) return sp_oint_of(0);
  if (errno == EINPROGRESS || errno == EALREADY) {
    if (!exc) return sp_oint_nil();
    sp_raise_cls("IO::EINPROGRESSWaitWritable", "operation in progress - connect(2) would block");
  }
  /* Already connected answers 0, the same as the two-argument form above and
     the same as CRuby answers here -- measured on both, in both exception
     modes, including a second connect to a different address. */
  if (errno == EISCONN) return sp_oint_of(0);
  sp_file_raise_errno("connect", "");
  return sp_oint_of(-1);
}

/* TCPServer#accept: park cooperatively for a pending connection first -- a
   blocking accept would stall the whole green-thread scheduler -- then wrap the
   new descriptor. Only a socket handle answers it. */
sp_File *sp_sock_accept(sp_File *f) {SP_GC_ROOT(f);
  extern int sp_net_accept_unless(int sfd, const unsigned char *cancel);
  if (!f || !f->is_sock)
    sp_raise_cls("NoMethodError",
                 sp_sprintf("undefined method 'accept' for an instance of %s", sp_io_kind_name(f)));
  SP_IO_OPEN(f);
  sp_io_wait_readable(f);
  /* the one use that spans a park: another acceptor can take the pending
     connection first, and the accept then waits for the next one, a wait
     the close cancels */
  int fd = sp_net_accept_unless(fileno(sp_io_use(f)), &f->closed);
  { int e = errno; sp_io_unuse(f); errno = e; }
  if (fd < 0 && SP_ATOMIC_LOAD(&f->closed, __ATOMIC_RELAXED)) sp_raise_cls("IOError", "stream closed in another thread");
  return sp_io_fdopen_sock(fd, SPL("tcp"));
}

/* `#<File:/etc/passwd>` for a path-backed handle, `#<IO:<STDOUT>>` for a
   standard stream. The path is what tells the two apart, the same way #class
   renders them (a stream's path is bracketed). */
const char *sp_File_inspect(sp_File *f) {SP_GC_ROOT(f);
  /* A NULL handle IS nil -- the readiness family answers one on timeout -- so
     it inspects as nil, the way sp_io_kind_name already names it NilClass. It
     rendered as a closed IO instead, which reads wrong in a puts even though
     nil? and truthiness were right (#4316). A handle that is merely CLOSED is
     a different thing and keeps its own rendering below. */
  if (!f) return SPL("nil");
  const char *p = f->path ? f->path : "";
  const char *cls = (p[0] && p[0] != '<') ? "File" : "IO";
  if (!f->fp || f->closed) return p[0] ? sp_sprintf("#<%s:%s (closed)>", cls, p)
                          : sp_sprintf("#<IO:(closed)>");
  /* a pipe end or a wrapped descriptor has no path: CRuby names the fd */
  if (!p[0]) return sp_sprintf("#<IO:fd %d>", fileno(f->fp));
  return sp_sprintf("#<%s:%s>", cls, p);
}

static void sp_File_puts_n(sp_File *f, const char *s, size_t n) {
  SP_IO_OPEN(f);
  if (f->is_sock) {
    sp_sock_write(f, s, n);
    if (n == 0 || s[n - 1] != '\n') sp_sock_write(f, "\n", 1);
    return;
  }
  fwrite(s, 1, n, f->fp);
  if (n == 0 || s[n - 1] != '\n') fputc('\n', f->fp);
}
void sp_File_puts(sp_File *f, const char *s) {SP_GC_ROOT(f);SP_GC_ROOT_STR(s);
  if (!s) { SP_IO_OPEN(f); return; }
  sp_File_puts_n(f, s, strlen(s));
}
/* Binary-safe puts, the pair of sp_File_write_bin: sizes the operand with the
   header length, so an embedded NUL reaches the descriptor and the trailing
   newline is decided by the real last byte. Reads s[-1], so it is for
   CODEGEN-emitted String values only -- the emitter picks it exactly where
   the write arm picks sp_File_write_bin. */
void sp_File_puts_bin(sp_File *f, const char *s) {SP_GC_ROOT(f);SP_GC_ROOT_STR(s);
  /* a nil String arrives as NULL and prints as `puts nil` does: a newline */
  if (!s) { sp_File_puts_n(f, "", 0); return; }
  sp_File_puts_n(f, s, sp_str_byte_len(s));
}

void sp_File_print(sp_File *f, const char *s) {SP_GC_ROOT(f);SP_GC_ROOT_STR(s);
  SP_IO_OPEN(f);
  if (!s) return;
  if (f->is_sock) { sp_sock_write(f, s, strlen(s)); return; }
  fputs(s, f->fp);
}

sp_int sp_File_flush(sp_File *f) {
  SP_IO_OPEN(f);
  fflush(f->fp);
  return 0;
}

sp_bool sp_File_eof_p(sp_File *f) {
  SP_IO_OPEN(f);
  sp_io_wait_readable(f);
  int c = SP_SIG_BLOCKING(fgetc(f->fp));
  if (c == EOF) return TRUE;
  ungetc(c, f->fp);
  return FALSE;
}

sp_int sp_File_seek(sp_File *f, sp_int off, sp_int whence) {
  SP_IO_OPEN(f);
  /* whence uses the Ruby IO::SEEK_* values (0/1/2), mapped explicitly so we
     never depend on the platform's SEEK_SET/CUR/END numbering. fseeko/ftello
     take off_t rather than fseek's long, so offsets past 2GB survive even
     where long is 32-bit. */
  int w = (whence == 1) ? SEEK_CUR : (whence == 2) ? SEEK_END : SEEK_SET;
  /* a refused seek is the descriptor's error, not a -1 return: a negative
     absolute offset is Errno::EINVAL in CRuby, and a caller that rescues
     Errno read the -1 as a successful seek (#4623) */
  if (fseeko(f->fp, (off_t)off, w) != 0) sp_file_raise_errno("seek", f->path ? f->path : "");
  return 0;
}

sp_int sp_File_tell(sp_File *f) {
  SP_IO_OPEN(f);
  return (sp_int)ftello(f->fp);
}

sp_int sp_File_rewind(sp_File *f) {
  SP_IO_OPEN(f);
  rewind(f->fp);
  return 0;
}

/* ---- File metadata predicates ----
   libc / WinAPI only, no spinel-string allocation and no shared mutable
   state, so they live here rather than inline in spinel_rt.h. */
sp_bool sp_file_directory(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

sp_bool sp_file_file(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

sp_bool sp_file_symlink(const char *path) {
  struct stat st;
  return path && lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

/* map errno to the matching Errno:: class (see sp_io.h) */
SP_NORETURN void sp_file_raise_errno(const char *op, const char *path) {SP_GC_ROOT_STR(op);SP_GC_ROOT_STR(path);
  int e = errno;   /* read once, before the formatting can touch it */
  sp_raise_cls(sp_errno_class_name(e),
               sp_sprintf("%s @ %s - %s", strerror(e), op, path ? path : ""));
}

sp_bool sp_file_owned(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && st.st_uid == geteuid();
}
sp_bool sp_file_grpowned(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && st.st_gid == getegid();
}
sp_bool sp_file_setuid(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && (st.st_mode & S_ISUID) != 0;
}
sp_bool sp_file_setgid(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && (st.st_mode & S_ISGID) != 0;
}
sp_bool sp_file_sticky(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && (st.st_mode & S_ISVTX) != 0;
}
sp_bool sp_file_socket(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}
sp_bool sp_file_blockdev(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISBLK(st.st_mode);
}
sp_bool sp_file_chardev(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISCHR(st.st_mode);
}
/* world_readable? / world_writable?: the permission bits (0..0777) when the
   other-read / other-write bit is set, else nil (#3005) */
sp_oint sp_file_world_readable(const char *path) {
  struct stat st;
  if (!(path && stat(path, &st) == 0 && (st.st_mode & S_IROTH))) return sp_oint_nil();
  return sp_oint_of((sp_int)(st.st_mode & 0777));
}
sp_oint sp_file_world_writable(const char *path) {
  struct stat st;
  if (!(path && stat(path, &st) == 0 && (st.st_mode & S_IWOTH))) return sp_oint_nil();
  return sp_oint_of((sp_int)(st.st_mode & 0777));
}
sp_int sp_file_do_symlink(const char *oldp, const char *newp) {SP_GC_ROOT_STR(newp);
  if (symlink(oldp, newp) != 0) sp_file_raise_errno("symlink", newp);
  return 0;
}
sp_int sp_file_do_link(const char *oldp, const char *newp) {SP_GC_ROOT_STR(newp);
  if (link(oldp, newp) != 0) sp_file_raise_errno("link", newp);
  return 0;
}
sp_int sp_file_umask(sp_int mask, int have_arg) {
  if (have_arg) return (sp_int)umask((mode_t)mask);
  mode_t cur = umask(0);   /* read is destructive; restore immediately */
  umask(cur);
  return (sp_int)cur;
}
sp_int sp_file_mkfifo(const char *path, sp_int mode) {SP_GC_ROOT_STR(path);
  if (mkfifo(path, (mode_t)mode) != 0) sp_file_raise_errno("mkfifo", path);
  return 0;
}
/* File.utime / File.lutime. The times arrive as a whole second and a
   nanosecond REMAINDER, not as a double: a Time carries nanoseconds and a
   timestamp near 2^31 seconds leaves a double about 100ns of resolution, so
   the fraction was lost long before utimes' microseconds were (#4635).
   utimensat takes the pair as it stands; the `l` form is the same call with
   AT_SYMLINK_NOFOLLOW, the way lstat is to stat. */
static sp_int sp_file_utime_at(int64_t asec, int32_t ansec, int64_t msec, int32_t mnsec,
                               const char *path, int flags, const char *who) {
  struct timespec ts[2];
  ts[0].tv_sec = (time_t)asec; ts[0].tv_nsec = (long)ansec;
  ts[1].tv_sec = (time_t)msec; ts[1].tv_nsec = (long)mnsec;
  if (utimensat(AT_FDCWD, path, ts, flags) != 0) sp_file_raise_errno(who, path);
  return 1;
}
sp_int sp_file_utime_ns(int64_t asec, int32_t ansec, int64_t msec, int32_t mnsec,
                        const char *path) {SP_GC_ROOT_STR(path);
  return sp_file_utime_at(asec, ansec, msec, mnsec, path, 0, "utime");
}
sp_int sp_file_lutime_ns(int64_t asec, int32_t ansec, int64_t msec, int32_t mnsec,
                         const char *path) {SP_GC_ROOT_STR(path);
  return sp_file_utime_at(asec, ansec, msec, mnsec, path, AT_SYMLINK_NOFOLLOW, "lutime");
}

/* stat, not fopen: opening a FIFO for read blocks until a writer appears, so
   the old fopen probe hung File.exist? on a fresh mkfifo path (#3118). stat
   also answers true for directories, matching CRuby. */
sp_bool sp_file_exist(const char *path) { struct stat st; return path && stat(path, &st) == 0; }
/* unlink(2), not remove(3): remove would take a directory too, which
   File.delete refuses (EPERM here, EISDIR on Linux), as CRuby does. */
void sp_file_path_check(const char *path) {
  if (!path) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
}
void sp_file_delete(const char *path) {SP_GC_ROOT_STR(path);
  sp_file_path_check(path);
  if (unlink(path) != 0) sp_file_raise_errno("apply2files", path);
}
void sp_file_rename(const char *from, const char *to) {SP_GC_ROOT_STR(from);SP_GC_ROOT_STR(to);
  sp_file_path_check(from); sp_file_path_check(to);
  if (rename(from, to) != 0) {
    int e = errno;
    const char *pair = sp_sprintf("(%s, %s)", from, to);
    errno = e;
    sp_file_raise_errno("rb_file_s_rename", pair);
  }
}

/* --- IO instance methods that ride the underlying fd (#3038) ------------- */

/* IO#readbyte: like #getbyte but EOFError at end of file. */
sp_int sp_File_readbyte(sp_File *f) {
  SP_IO_OPEN(f);
  sp_io_wait_readable(f);
  int ch = SP_SIG_BLOCKING(fgetc(f->fp));
  if (ch == EOF) sp_raise_cls("EOFError", "end of file reached");
  return (sp_int)(unsigned char)ch;
}
/* IO#ungetbyte: push one byte back onto the read buffer; returns nil. */
void sp_File_ungetbyte(sp_File *f, sp_int byte) {
  SP_IO_OPEN(f);
  ungetc((int)(unsigned char)byte, f->fp);
}
/* IO#binmode?: true after #binmode, or for a handle opened in binary mode.
   A socket is binary too, as CRuby's is (its encoding is BINARY). */
sp_bool sp_File_binmode_p(sp_File *f) {
  SP_IO_OPEN(f);
  if (f->bin_flag || f->is_sock) return 1;
  return f->mode && strchr(f->mode, 'b') != NULL;
}
/* binmode makes the external encoding ASCII-8BIT again, whatever an earlier
   #set_encoding chose */
void sp_File_set_binmode(sp_File *f) { SP_IO_OPEN(f); f->bin_flag = 1; f->ext_enc = 0; }
/* The handle flags codegen used to read straight off the struct (#2792,
   #3131): a closed handle answers IOError for them as for everything else.
   #sync= keeps the flush a truthy value implies (#4229). */
sp_int sp_File_lineno(sp_File *f) { SP_IO_OPEN(f); return f->lineno; }
sp_int sp_File_set_lineno(sp_File *f, sp_int n) { SP_IO_OPEN(f); f->lineno = n; return n; }
sp_bool sp_File_sync_p(sp_File *f) { SP_IO_OPEN(f); return (f->is_sock || f->sync_on) ? 1 : 0; }
sp_bool sp_File_set_sync(sp_File *f, sp_bool on) {
  SP_IO_OPEN(f);
  f->sync_on = on ? 1 : 0;
  /* A sync handle is unbuffered, as in CRuby: every write reaches the
     descriptor before the call returns. Kernel#puts writes straight through
     stdio, so the flag alone did nothing for a file or a pipe -- a test that
     set `$stdout.sync = true` and was killed by the CI timeout left an EMPTY
     output, with no clue where it had hung. The stream is flushed first so
     the buffer swap loses nothing. */
  fflush(f->fp);
  if (f->fp != sp_io_closed_sentinel())
    setvbuf(f->fp, NULL, on ? _IONBF : (isatty(fileno(f->fp)) ? _IOLBF : _IOFBF), 0);
  return on;
}
sp_bool sp_File_autoclose_p(sp_File *f) { SP_IO_OPEN(f); return !f->no_autoclose; }
void sp_File_set_autoclose(sp_File *f, sp_bool on) { SP_IO_OPEN(f); f->no_autoclose = !on; }
/* IO#reopen(io): rebind this handle's descriptor onto the other stream. */
sp_File *sp_File_reopen_io(sp_File *f, sp_File *other) {SP_GC_ROOT(f);SP_GC_ROOT(other); sp_gc_wb((void*)f);
  if (!f || !other) return f;
  /* a closed handle on either side is CRuby's IOError, not a quiet no-op */
  SP_IO_OPEN(f);
  SP_IO_OPEN(other);
  fflush(f->fp);
  fflush(other->fp);
  if (dup2(fileno(other->fp), fileno(f->fp)) < 0)
    sp_file_raise_errno("reopen", other->path ? other->path : "");
  f->path = other->path;
  f->mode = other->mode;
  f->lineno = 0;
  return f;
}
/* The fopen mode a descriptor's own access mode answers ("r", "w", "a" or
   "r+"), as CRuby derives it. A marked literal: the handle keeps the pointer
   as f->mode, and the collector reads its marker byte. */
const char *sp_io_fd_mode(int fd) {
  int fl = fcntl(fd, F_GETFL);
  int acc = fl >= 0 ? (fl & O_ACCMODE) : O_RDONLY;
  if (acc == O_WRONLY) return (fl & O_APPEND) ? SPL("a") : SPL("w");
  if (acc == O_RDWR) return SPL("r+");
  return SPL("r");
}

/* IO#dup / #clone: a new handle on a dup(2) of the descriptor, as CRuby makes
   it. The two share the open file (its offset and status flags) but not the
   descriptor, so closing or reopening one leaves the other as it was:
   `orig = $stdout.dup; $stdout.reopen(f); ...; $stdout.reopen(orig)` puts
   stdout back. The original's stream is first brought to its logical
   position -- buffered output written, read-ahead given back to the
   descriptor -- so the copy starts where the original stands. glibc's and
   musl's fflush do both; BSD's only writes. A descriptor with no offset (a
   pipe) keeps its read-ahead with the original, as CRuby's does. The copy is close-on-exec, and sync where the
   original is ($stderr always is). A File::Stat shares the handle type and
   has no descriptor: it is answered as itself, as before. */
sp_File *sp_File_dup(sp_File *f, int keep_frozen) {SP_GC_ROOT(f);
  if (f && f->mode && (strcmp(f->mode, "stat") == 0 || strcmp(f->mode, "lstat") == 0)) return f;
  SP_IO_OPEN(f);
  off_t pos = ftello(f->fp);
  /* Buffered output that cannot be written is an error here, as in CRuby.
     Only a writable stream is checked: BSD stdio's fflush on a read-only
     stream fails with EBADF, whatever the descriptor's own mode. */
  if (fflush(f->fp) != 0 && f->mode && strpbrk(f->mode, "wa+"))
    sp_file_raise_errno("rb_sys_fail_on_write", f->path ? f->path : "");
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  /* BSD stdio's fflush leaves a read stream's read-ahead in place: give it
     back to the descriptor by hand */
  if (pos >= 0 && lseek(fileno(f->fp), 0, SEEK_CUR) != pos) {
    fpurge(f->fp);
    lseek(fileno(f->fp), pos, SEEK_SET);
  }
#else
  (void)pos;
#endif
  int d = dup(fileno(f->fp));
  if (d < 0) sp_file_raise_errno("dup", f->path ? f->path : "");
  { int fdf = fcntl(d, F_GETFD); if (fdf >= 0) fcntl(d, F_SETFD, fdf | FD_CLOEXEC); }
  sp_File *g = sp_io_fdopen_ex(d, sp_io_fd_mode(d), 1); SP_GC_ROOT(g);
  sp_gc_wb((void *)g);
  g->path = f->path;
  g->mode = f->mode;
  g->lineno = f->lineno;
  g->bin_flag = f->bin_flag;
  g->ext_enc = f->ext_enc;
  g->is_sock = f->is_sock;
  g->rev_lookup = f->rev_lookup;
  g->is_file = f->is_file;
  g->frozen = keep_frozen ? f->frozen : 0;
  g->timeout = SP_ATOMIC_LOAD(&f->timeout, __ATOMIC_ACQUIRE);
  if (f->sync_on || f->fp == stderr) sp_File_set_sync(g, 1);
  return g;
}
/* IO#close_on_exec? / #close_on_exec= via the FD_CLOEXEC descriptor flag. */
sp_bool sp_File_close_on_exec_p(sp_File *f) {
  SP_IO_OPEN(f);
  int fl = fcntl(fileno(f->fp), F_GETFD);
  return fl >= 0 && (fl & FD_CLOEXEC) != 0;
}
void sp_File_set_close_on_exec(sp_File *f, sp_bool on) {
  SP_IO_OPEN(f);
  int fd = fileno(f->fp);
  int fl = fcntl(fd, F_GETFD);
  if (fl < 0) return;
  fcntl(fd, F_SETFD, on ? (fl | FD_CLOEXEC) : (fl & ~FD_CLOEXEC));
}
/* IO#fcntl(cmd, arg=0): the raw descriptor command. */
sp_int sp_File_fcntl(sp_File *f, sp_int cmd, sp_int arg) {SP_GC_ROOT(f);
  SP_IO_OPEN(f);
  int r = fcntl(fileno(f->fp), (int)cmd, (long)arg);
  if (r < 0) sp_file_raise_errno("fcntl", f->path ? f->path : "");
  return (sp_int)r;
}
/* IO#pwrite(str, offset): write without moving the file position. */
static sp_int sp_File_pwrite_n(sp_File *f, const char *s, size_t n, sp_int off) {
  SP_IO_OPEN(f);
  fflush(f->fp);
  ssize_t put = pwrite(fileno(f->fp), s ? s : "", n, (off_t)off);
  if (put < 0) sp_file_raise_errno("pwrite", f->path ? f->path : "");
  return (sp_int)put;
}
sp_int sp_File_pwrite(sp_File *f, const char *s, sp_int off) {SP_GC_ROOT(f);SP_GC_ROOT_STR(s);
  return sp_File_pwrite_n(f, s, s ? strlen(s) : 0, off);
}
/* Binary-safe pwrite, the pair of sp_File_write_bin: sizes the operand with
   the header length, so an embedded NUL reaches the descriptor instead of
   cutting the write short. Reads s[-1], so it is for CODEGEN-emitted String
   values only -- the emitter picks it exactly where the write arm picks
   sp_File_write_bin. */
sp_int sp_File_pwrite_bin(sp_File *f, const char *s, sp_int off) {SP_GC_ROOT(f);SP_GC_ROOT_STR(s);
  return sp_File_pwrite_n(f, s, s ? sp_str_byte_len(s) : 0, off);
}
/* IO#advise(sym, offset=0, len=0): a hint, and nil either way. POSIX
   fadvise is Linux-ish; where it is absent the hint is simply dropped. */
void sp_File_advise(sp_File *f, const char *kind, sp_int off, sp_int len) {
  SP_IO_OPEN(f);
#ifdef POSIX_FADV_NORMAL
  int fd = fileno(f->fp);
  int a = POSIX_FADV_NORMAL;
  if (!kind) return;
  if (!strcmp(kind, "sequential")) a = POSIX_FADV_SEQUENTIAL;
  else if (!strcmp(kind, "random")) a = POSIX_FADV_RANDOM;
  else if (!strcmp(kind, "willneed")) a = POSIX_FADV_WILLNEED;
  else if (!strcmp(kind, "dontneed")) a = POSIX_FADV_DONTNEED;
  else if (!strcmp(kind, "noreuse")) a = POSIX_FADV_NOREUSE;
  posix_fadvise(fd, (off_t)off, (off_t)len, a);
#else
  (void)kind; (void)off; (void)len;
#endif
}
/* IO#close_read / #close_write. A plain file is not duplex, so half-closing
   the side it does not have raises; half-closing the side it IS just closes
   the handle, which is what CRuby does. */
void sp_File_close_half(sp_File *f, sp_bool reading) {SP_GC_ROOT(f);
  /* a closed File or pipe answers nil, as CRuby's IO#close_read and
     #close_write do, ahead of the duplex check; a closed socket raises, as
     BasicSocket's do */
  if (f && f->closed) {
    if (f->is_sock) sp_io_raise_closed();
    return;
  }
  const char *m = (f && f->mode) ? f->mode : "r";
  sp_bool writable = strchr(m, 'w') || strchr(m, 'a') || strchr(m, '+');
  sp_bool readable = strchr(m, 'r') || strchr(m, '+');
  if (reading ? !readable : !writable)
    sp_raise_cls("IOError", reading ? "closing non-duplex IO for reading"
                                    : "closing non-duplex IO for writing");
  if (reading ? writable : readable)
    sp_raise_cls("IOError", reading ? "closing non-duplex IO for reading"
                                    : "closing non-duplex IO for writing");
  if (f && f->fp && !f->closed) sp_io_close_handle(f);
}
/* IO#reopen(path, mode): rebind the handle to another file. */
sp_File *sp_File_reopen(sp_File *f, const char *path, const char *mode) {SP_GC_ROOT(f);SP_GC_ROOT_STR(path);SP_GC_ROOT_STR(mode); sp_gc_wb((void*)f);
  if (!f) return f;
  /* with no mode the handle keeps its own, as CRuby's does: a "w" handle
     reopens for writing (and truncates), an "rb" one stays binary. fopen
     takes the access part of it only: no encoding suffix, no "x" */
  char km[8]; const char *om = mode && mode[0] ? mode : "r";
  if (!(mode && mode[0]) && f->mode && (f->mode[0] == 'r' || f->mode[0] == 'w' || f->mode[0] == 'a')) {
    int n = 0;
    for (const char *q = f->mode; *q && *q != ':' && n < (int)sizeof km - 1; q++)
      if (*q == 'r' || *q == 'w' || *q == 'a' || *q == '+' || *q == 'b') km[n++] = *q;
    km[n] = 0;
    om = km;
  }
  /* a closed handle carries the shared sentinel, which must not be
     reopened: it gets a fresh stream, once no thread is still inside a
     read of the old one (sp_io_use), whose release would close the new.
     The handle stays reserved until the new stream is in place, so no use
     is taken of the sentinel meanwhile and no second #reopen starts, and
     goes back to closed if the open fails: it was left with neither, and
     every later #reopen of it raised */
  int reserved = 0;
  if (f->closed) {
    unsigned int u = SP_IO_CLOSING | SP_IO_RETIRED;
    if (!SP_ATOMIC_CAS(&f->uses, &u, SP_IO_CLOSING | SP_IO_RETIRED | SP_IO_REOPENING, 0,
                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
      sp_raise_cls("IOError", "stream closed in another thread");
    reserved = 1;
  }
  FILE *nf = (f->closed || !f->fp) ? fopen(path ? path : "", om)
                                   : freopen(path ? path : "", om, f->fp);
  if (!nf) {
    if (reserved) SP_ATOMIC_STORE(&f->uses, SP_IO_CLOSING | SP_IO_RETIRED, __ATOMIC_RELEASE);
    sp_file_raise_errno("reopen", path ? path : "");
  }
  SP_ATOMIC_STORE(&f->fp, nf, __ATOMIC_RELAXED);
  SP_ATOMIC_STORE(&f->closed, 0, __ATOMIC_RELAXED);
  if (reserved) SP_ATOMIC_STORE(&f->uses, 0, __ATOMIC_RELEASE);
  f->path = path;
  /* a mode given replaces the handle's, as CRuby's does: an earlier
     #binmode or #set_encoding no longer decides what a read answers */
  if (mode && mode[0]) { f->bin_flag = 0; f->ext_enc = 0; }
  if (om != km) f->mode = om;
  f->lineno = 0;
  return f;
}
