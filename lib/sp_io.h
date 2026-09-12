/* sp_io.h -- File / IO handle surface.
 *
 * sp_File is a stdio FILE* plus its (GC-managed) path/mode strings,
 * shared between the generated translation unit and lib/sp_io.c, which
 * holds the allocation-free handle ops. The string-returning readers
 * (sp_File_gets / _read / _read_n / _path) stay inline in spinel_rt.h
 * because they allocate via the hot static sp_str_alloc; moving them
 * would split the per-TU string heap. */
#ifndef SP_IO_H
#define SP_IO_H

#include <stdio.h>
#include "sp_types.h"   /* sp_int, sp_bool */
#include "sp_array.h"   /* sp_IntArray (for #winsize) */

typedef struct {
  FILE *fp; const char *path; const char *mode; sp_int lineno;
  unsigned char bin_flag;      /* #binmode was called (#3131) */
  unsigned char no_autoclose;  /* #autoclose = false (#3131) */
  unsigned char is_sock;       /* a socket handle: writes bypass stdio (#2922) */
  unsigned char sync_on;       /* #sync is true for this handle: a write reaches
                                  the descriptor at once. A socket is always
                                  sync (its writes bypass stdio); IO.pipe's
                                  WRITE end is sync in CRuby too, and without
                                  the flag a byte sat in stdio where a reader
                                  -- or an IO.select on the other end -- could
                                  not see it (#4263). */
  unsigned char frozen;        /* Object#freeze; kept here, not in the GC header,
                                  because the standard streams are static storage
                                  with no header to flip (sp_io_stdout) */
  unsigned char wnonblock;     /* write side of this handle is O_NONBLOCK:
                                  0 not asked, 1 yes, 2 could not (#4307) */
  unsigned char park;          /* readiness-park kind, computed on the first
                                  read: 0 not yet asked, 1 never (a regular
                                  file is always ready), 2 park before a read
                                  that could block (#4307) */
  int fno_plus1;               /* IO.for_fd(fd, autoclose: false) wraps a dup(2)
                                  of fd so close/fin never touch the caller's
                                  descriptor; this carries the ORIGINAL fd (+1,
                                  0 = unset) so #fileno answers it (#4208) */
} sp_File;

/* Object#frozen? / #freeze on a handle. A nil slot is nil's answer: frozen. */
sp_bool sp_io_frozen(sp_File *f);
sp_File *sp_io_freeze(sp_File *f);
/* Object#== on two handles: identity, except that two File::Stat handles
   compare as Comparable does for File::Stat -- by modification time. <=> reads
   the same way (0 / nil, or the mtime order of two stats), boxed; to_s is
   Object's #<Class:0xADDR> under the class the handle presents as. */
sp_bool sp_io_eq(sp_File *a, sp_File *b);
sp_RbVal sp_io_cmp(sp_File *a, sp_RbVal other);
const char *sp_io_to_s(sp_File *f);

/* File.open(path, mode) -> GC-managed handle (block form is codegen-only). */
sp_File *sp_File_open(const char *path, const char *mode);
/* pipe(2) wrapper. 0 ok, -1 error. */
int sp_io_make_pipe(int fds[2]);
/* IO.pipe end: wrap a raw pipe fd in a GC-managed sp_File. */
sp_File *sp_io_fdopen(int fd, const char *mode);
sp_File *sp_io_fdopen_ex(int fd, const char *mode, int owns_fd);
sp_File *sp_File_open_perm(const char *path, const char *mode, sp_int perm);
/* Wrap a connected/listening socket fd. Reads stay on the buffered FILE* so
   #gets and friends work; writes bypass stdio straight to write(2), matching
   CRuby sockets' sync = true. `kind` labels the handle ("tcp", "tcpserver",
   ...) for #class rendering. (#2922) */
sp_File *sp_io_fdopen_sock(int fd, const char *kind);
void sp_io_wait_readable(sp_File *f);
/* Park until the handle can take bytes (a socket, or an IO.pipe write end). */
void sp_io_wait_writable(sp_File *f);
/* Bytes stdio already holds for this stream (readable without a read(2)). */
size_t sp_io_stdio_buffered(FILE *fp);
/* IO#read with no count on a handle whose read can block: fill to EOF,
   parking between refills instead of sitting in the kernel (#4307). */
const char *sp_slurp_stream_parked(sp_File *f);
sp_int sp_File_write(sp_File *f, const char *s);
sp_int sp_File_write_bin(sp_File *f, const char *s);
sp_int sp_File_syswrite(sp_File *f, const char *s, size_t n);
sp_int sp_File_close(sp_File *f);

/* Every operation on a handle whose descriptor is gone raises IOError in
   CRuby. Some of the read side already did; the write side and the position
   queries answered a seed instead, so a write to a closed socket looked like
   a successful send of zero bytes and the loss went unnoticed. The byte and
   character readers, the flag accessors and the descriptor queries answered
   nil, EOF or a default the same way, so a program that kept using a handle
   it had closed ran on silently. #closed?, #close, #inspect and #path stay
   exempt: they are the ones that are meant to work on a closed handle. The
   metadata a File answers by its path (#size, #mtime, #chmod ...) still
   answers here where CRuby raises: a File::Stat rides this same struct with
   no descriptor, so that check cannot be this one. */
SP_NORETURN SP_COLD void sp_io_raise_closed(void);
#define SP_IO_OPEN(f) do { if (!(f) || !(f)->fp) sp_io_raise_closed(); } while (0)
sp_bool sp_File_closed_p(sp_File *f);
/* The handle flags codegen read straight off the struct (#lineno, #sync,
   #autoclose? and their setters): through here so a closed handle answers
   IOError for them too. #lineno= and #sync= answer their value, which is the
   expression's; #autoclose='s emitter keeps its own operand. */
sp_int  sp_File_lineno(sp_File *f);
sp_int  sp_File_set_lineno(sp_File *f, sp_int n);
sp_bool sp_File_sync_p(sp_File *f);
sp_bool sp_File_set_sync(sp_File *f, sp_bool on);
sp_bool sp_File_autoclose_p(sp_File *f);
void    sp_File_set_autoclose(sp_File *f, sp_bool on);
const char *sp_File_inspect(sp_File *f);
const char *sp_io_kind_name(sp_File *f);
sp_File *sp_sock_accept(sp_File *f);
sp_File *sp_sock_accept_nb(sp_File *f, sp_bool exc);
const char *sp_sock_read_nb(sp_File *f, sp_int len, sp_bool exc, sp_bool is_recv, sp_bool *eof);
sp_int sp_sock_write_nb(sp_File *f, const char *data, sp_bool exc);
sp_int sp_sock_write_nb_bin(sp_File *f, const char *data, sp_bool exc);
sp_int sp_sock_connect_nb(sp_File *f, const char *host, sp_int port, sp_bool exc);
/* Addrinfo-form connect_nonblock (already-resolved endpoint) */
sp_int sp_sock_connect_nb_sa(sp_File *f, const char *sa, sp_int salen,
                                 sp_bool exc);
sp_bool sp_io_is_a(sp_File *f, const char *cls);
sp_bool sp_io_instance_of(sp_File *f, const char *cls);
sp_File *sp_sock_udp_new(sp_int family);
const char *sp_sock_gethostname(void);
sp_PolyArray *sp_sock_getaddrinfo(const char *host, sp_int port);
sp_Addrinfo *sp_sock_address(sp_File *f, sp_int peer);
/* Packed sockaddr strings: what Socket.sockaddr_in / Socket.pack_sockaddr_in,
   Socket.pack_sockaddr_un and Addrinfo#to_sockaddr answer, and what the 1-arg
   connect_nonblock takes. Byte strings -- they carry NUL. */
const char *sp_sock_pack_sockaddr_in(sp_int port, const char *host);
const char *sp_sock_pack_sockaddr_un(const char *path);
const char *sp_addrinfo_to_sockaddr(sp_Addrinfo *a);
sp_PolyArray *sp_sock_unpack_sockaddr_in(const char *sa);
sp_File *sp_sock_new(sp_int domain, sp_int type, sp_int proto);
sp_File *sp_sock_pair_end(sp_int domain, sp_int type, sp_int proto, sp_int which);
sp_File *sp_sock_unix_server(const char *path);
sp_File *sp_sock_unix_connect(const char *path);
sp_int sp_sock_bind(sp_File *f, const char *host, sp_int port);
sp_int sp_sock_connect(sp_File *f, const char *host, sp_int port);
sp_int sp_sock_send(sp_File *f, const char *data, sp_int len, const char *host, sp_int port);
sp_int sp_sock_shutdown(sp_File *f, sp_int how);
sp_int sp_sock_const(const char *n);
const char *sp_sock_recv(sp_File *f, sp_int len);
const char *sp_sock_recvfrom(sp_File *f, sp_int len, const char **ip_out, sp_int *port_out);
sp_int sp_sock_setsockopt(sp_File *f, sp_int level, sp_int opt, sp_int value);
sp_SockOpt *sp_sock_getsockopt(sp_File *f, sp_int level, sp_int opt);
sp_int sp_sock_listen(sp_File *f, sp_int backlog);
void sp_File_puts(sp_File *f, const char *s);
void sp_File_print(sp_File *f, const char *s);
sp_int sp_File_flush(sp_File *f);
sp_bool sp_File_eof_p(sp_File *f);
/* IO instance methods riding the underlying fd (#3038). */
sp_int sp_File_readbyte(sp_File *f);
void sp_File_ungetbyte(sp_File *f, sp_int byte);
sp_bool sp_File_binmode_p(sp_File *f);
void sp_File_set_binmode(sp_File *f);
sp_File *sp_File_reopen_io(sp_File *f, sp_File *other);
sp_bool sp_File_close_on_exec_p(sp_File *f);
void sp_File_set_close_on_exec(sp_File *f, sp_bool on);
sp_int sp_File_fcntl(sp_File *f, sp_int cmd, sp_int arg);
sp_int sp_File_pwrite(sp_File *f, const char *s, sp_int off);
void sp_File_advise(sp_File *f, const char *kind, sp_int off, sp_int len);
void sp_File_close_half(sp_File *f, sp_bool reading);
sp_File *sp_File_reopen(sp_File *f, const char *path, const char *mode);
sp_int sp_File_seek(sp_File *f, sp_int off, sp_int whence); /* #seek -- whence: 0=SET 1=CUR 2=END */
sp_int sp_File_tell(sp_File *f);       /* #tell / #pos -- ftello, -1 on closed */
sp_int sp_File_rewind(sp_File *f);     /* #rewind */
sp_bool sp_File_tty_p(sp_File *f);     /* #tty? / #isatty -- isatty(fileno) */
sp_int sp_File_fileno(sp_File *f);     /* #fileno */
sp_IntArray *sp_File_winsize(sp_File *f); /* #winsize -> [rows, cols] (ioctl, or [0,0]) */

/* STDOUT / STDERR as shared IO handles wrapping the C stdout/stderr streams.
   The handle is a function-local static (stdout/stderr are not constant
   initializers) and is never closed. */
sp_File *sp_io_stdout(void);
sp_File *sp_io_stderr(void);
sp_File *sp_io_stdin(void);

/* File metadata predicates (libc/WinAPI only; defined in sp_io.c). */
sp_bool sp_file_directory(const char *path);
sp_bool sp_file_file(const char *path);
sp_bool sp_file_exist(const char *path);
sp_bool sp_file_symlink(const char *path);
sp_bool sp_file_owned(const char *path);
sp_bool sp_file_grpowned(const char *path);
sp_bool sp_file_setuid(const char *path);
sp_bool sp_file_setgid(const char *path);
sp_bool sp_file_sticky(const char *path);
sp_bool sp_file_socket(const char *path);
sp_bool sp_file_blockdev(const char *path);
sp_bool sp_file_chardev(const char *path);
sp_int sp_file_world_readable(const char *path);
sp_int sp_file_world_writable(const char *path);
sp_int sp_file_do_symlink(const char *oldp, const char *newp);
sp_int sp_file_do_link(const char *oldp, const char *newp);
sp_int sp_file_umask(sp_int mask, int have_arg);
sp_int sp_file_mkfifo(const char *path, sp_int mode);
sp_int sp_file_utime(double atime, double mtime, const char *path);
const char *sp_file_readlink(const char *path);  /* defined in sp_cold.c */
void sp_file_delete(const char *path);
void sp_file_rename(const char *from, const char *to);
/* A filesystem call that failed raises the Errno:: class CRuby raises for the
   errno, with CRuby's message shape "<strerror> @ <op> - <path>". op is the
   label after the @: for the File and Dir class methods it is CRuby's own
   entry-point name (dir_s_mkdir, apply2files, rb_file_s_rename ...), so a
   program matching on the text reads the same words; the socket calls and
   the older File sites (readlink, symlink, mkfifo ...) pass the syscall's
   name. An errno with no class here raises SystemCallError. */
SP_NORETURN void sp_file_raise_errno(const char *op, const char *path);
/* A nil path (a NULL string at run time) is CRuby's TypeError, raised before
   any syscall sees it; the multi-path File.delete checks every path first. */
void sp_file_path_check(const char *path);

#include <dirent.h>
/* Dir handle (Dir.open / Dir.each_child ...): ops live in lib/sp_cold.c. */
typedef struct { DIR *dp; const char *path; } sp_Dir;
/* The Dir counterpart of SP_IO_OPEN: reading or positioning a closed handle
   is IOError "closed directory" in CRuby; #close, #path and #inspect work. */
SP_NORETURN SP_COLD void sp_dir_raise_closed(void);
#define SP_DIR_OPEN(d) do { if (!(d) || !(d)->dp) sp_dir_raise_closed(); } while (0)

/* Object's to_s and <=> on the handle (sp_cold.c), declared here with the
   struct because the poly to_s and spaceship in spinel_rt.h call them. */
const char *sp_Dir_to_s(sp_Dir *d);
sp_RbVal sp_Dir_cmp(sp_Dir *d, sp_RbVal other);

/* ---- sp_io_pipe/sysopen relocated from spinel_rt.h (0 optcarrot uses). ---- */
sp_PolyArray *sp_io_pipe(void);
sp_int sp_io_sysopen(const char *path, sp_int flags, sp_int perm);

/* IO.select accepts anything that answers #to_io, which is how CRuby lets a
   wrapper -- a protocol object holding a socket -- be waited on. The runtime
   cannot dispatch a user method itself, so codegen emits the cls_id switch and
   main() installs it here, the same shape as sp_user_exc_parent_fn and the
   sp_json_*_fn hooks. NULL when the program defines no #to_io, which is when
   an element that is not an IO is the TypeError it always was. */
extern sp_File *(*sp_user_to_io_hook)(sp_RbVal);

#endif
