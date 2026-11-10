/* sp_win32.c -- the POSIX calls the runtime makes, over the Win32 API and the
   UCRT. See sp_win32.h for the shape of the shim; this file and
   sp_win32_net.c (sockets, poll, and fd I/O that has to tell a socket from a
   file) are the only ones that include <windows.h>.

   <windows.h> comes first, before any shim header: the shims define macros
   (read, stat, open, ...) that must not rename anything inside the SDK's
   headers. From then on this file calls the CRT and Win32 names directly
   (_read, _wopen, CreateFileW) and defines the sp_w32_ ones the macros
   point at. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <share.h>
#include <wchar.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <locale.h>
#include <dirent.h>
#include <fnmatch.h>
#include <pwd.h>
#include <grp.h>
#include <dlfcn.h>
#include <spawn.h>
#include <stdio_ext.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/file.h>
#include <pthread.h>
#include "sp_win32.h"

#ifndef IO_REPARSE_TAG_AF_UNIX
#define IO_REPARSE_TAG_AF_UNIX 0x80000023
#endif
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

/* sp_win32_net.c: whether a handle is a socket's */
int sp_w32_is_socket_handle(HANDLE h);

/* the CRT originals, past the shim's macros */
#undef read
#undef write
#undef close
#undef dup
#undef dup2
#undef pipe
#undef isatty
#undef readlink
#undef symlink
#undef link
#undef unlink
#undef access
#undef chdir
#undef rmdir
#undef truncate
#undef getcwd
#undef realpath
#undef system
#undef mkstemp
#undef popen
#undef pclose
#undef fopen
#undef freopen
#undef fdopen
#undef remove
#undef rename
#undef open
#undef mkdir
#undef chmod
#undef strerror
#undef strndup
#undef nanosleep
#undef execv
#undef execvp
#undef execve
#undef execl
#undef pthread_sigmask

/* ===================================================================== */
/* errno from a Win32 error                                              */
/* ===================================================================== */

char *sp_w32_strndup(const char *s, size_t n) {
  size_t len = strnlen(s, n);
  char *r = malloc(len + 1);
  if (!r) { errno = ENOMEM; return NULL; }
  memcpy(r, s, len);
  r[len] = '\0';
  return r;
}

/* glibc's strerror texts */
char *sp_w32_strerror(int e) {
  switch (e) {
    case 0: return (char *)"Success";
    case EPERM: return (char *)"Operation not permitted";
    case ENOENT: return (char *)"No such file or directory";
    case ESRCH: return (char *)"No such process";
    case EINTR: return (char *)"Interrupted system call";
    case EIO: return (char *)"Input/output error";
    case ENXIO: return (char *)"No such device or address";
    case E2BIG: return (char *)"Argument list too long";
    case ENOEXEC: return (char *)"Exec format error";
    case EBADF: return (char *)"Bad file descriptor";
    case ECHILD: return (char *)"No child processes";
    case EAGAIN: return (char *)"Resource temporarily unavailable";
    case ENOMEM: return (char *)"Cannot allocate memory";
    case EACCES: return (char *)"Permission denied";
    case EFAULT: return (char *)"Bad address";
    case EBUSY: return (char *)"Device or resource busy";
    case EEXIST: return (char *)"File exists";
    case EXDEV: return (char *)"Invalid cross-device link";
    case ENODEV: return (char *)"No such device";
    case ENOTDIR: return (char *)"Not a directory";
    case EISDIR: return (char *)"Is a directory";
    case EINVAL: return (char *)"Invalid argument";
    case ENFILE: return (char *)"Too many open files in system";
    case EMFILE: return (char *)"Too many open files";
    case ENOTTY: return (char *)"Inappropriate ioctl for device";
    case EFBIG: return (char *)"File too large";
    case ENOSPC: return (char *)"No space left on device";
    case ESPIPE: return (char *)"Illegal seek";
    case EROFS: return (char *)"Read-only file system";
    case EMLINK: return (char *)"Too many links";
    case EPIPE: return (char *)"Broken pipe";
    case EDOM: return (char *)"Numerical argument out of domain";
    case ERANGE: return (char *)"Numerical result out of range";
    case EDEADLK: return (char *)"Resource deadlock avoided";
    case ENAMETOOLONG: return (char *)"File name too long";
    case ENOLCK: return (char *)"No locks available";
    case ENOSYS: return (char *)"Function not implemented";
    case ENOTEMPTY: return (char *)"Directory not empty";
    case EILSEQ: return (char *)"Invalid or incomplete multibyte or wide character";
    case EADDRINUSE: return (char *)"Address already in use";
    case EADDRNOTAVAIL: return (char *)"Cannot assign requested address";
    case EAFNOSUPPORT: return (char *)"Address family not supported by protocol";
    case EALREADY: return (char *)"Operation already in progress";
    case ECANCELED: return (char *)"Operation canceled";
    case ECONNABORTED: return (char *)"Software caused connection abort";
    case ECONNREFUSED: return (char *)"Connection refused";
    case ECONNRESET: return (char *)"Connection reset by peer";
    case EDESTADDRREQ: return (char *)"Destination address required";
    case EHOSTUNREACH: return (char *)"No route to host";
    case EINPROGRESS: return (char *)"Operation now in progress";
    case EISCONN: return (char *)"Transport endpoint is already connected";
    case ELOOP: return (char *)"Too many levels of symbolic links";
    case EMSGSIZE: return (char *)"Message too long";
    case ENETDOWN: return (char *)"Network is down";
    case ENETRESET: return (char *)"Network dropped connection on reset";
    case ENETUNREACH: return (char *)"Network is unreachable";
    case ENOBUFS: return (char *)"No buffer space available";
    case ENOPROTOOPT: return (char *)"Protocol not available";
    case ENOTCONN: return (char *)"Transport endpoint is not connected";
    case ENOTSOCK: return (char *)"Socket operation on non-socket";
    case ENOTSUP: return (char *)"Operation not supported";
    case EOVERFLOW: return (char *)"Value too large for defined data type";
    case EPROTONOSUPPORT: return (char *)"Protocol not supported";
    case EPROTOTYPE: return (char *)"Protocol wrong type for socket";
    case ETIMEDOUT: return (char *)"Connection timed out";
    case ETXTBSY: return (char *)"Text file busy";
    case EOWNERDEAD: return (char *)"Owner died";
    case ENOTRECOVERABLE: return (char *)"State not recoverable";
    case EBADMSG: return (char *)"Bad message";
    case EIDRM: return (char *)"Identifier removed";
    case ENODATA: return (char *)"No data available";
    case ENOLINK: return (char *)"Link has been severed";
    case ENOMSG: return (char *)"No message of desired type";
    case ENOSR: return (char *)"Out of streams resources";
    case ENOSTR: return (char *)"Device not a stream";
    case ETIME: return (char *)"Timer expired";
    case EPROTO: return (char *)"Protocol error";
    case ESTALE: return (char *)"Stale file handle";
    case EDQUOT: return (char *)"Disk quota exceeded";
    case ENOTBLK: return (char *)"Block device required";
    case EUSERS: return (char *)"Too many users";
    case ESHUTDOWN: return (char *)"Cannot send after transport endpoint shutdown";
    case ETOOMANYREFS: return (char *)"Too many references: cannot splice";
    case EHOSTDOWN: return (char *)"Host is down";
    case EPFNOSUPPORT: return (char *)"Protocol family not supported";
    case ESOCKTNOSUPPORT: return (char *)"Socket type not supported";
    case EREMOTE: return (char *)"Object is remote";
    case EMULTIHOP: return (char *)"Multihop attempted";
    case ENOMEDIUM: return (char *)"No medium found";
    default: {
      static __thread char buf[32];
      snprintf(buf, sizeof buf, "Unknown error %d", e);
      return buf;
    }
  }
}

int sp_w32_errno_of(DWORD e) {
  switch (e) {
    case ERROR_SUCCESS: return 0;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH: case ERROR_BAD_NET_NAME: case ERROR_BAD_PATHNAME:
    case ERROR_INVALID_NAME: case ERROR_MOD_NOT_FOUND: case ERROR_DIRECTORY:
      return ENOENT;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION:
    case ERROR_CURRENT_DIRECTORY: case ERROR_WRITE_PROTECT: case ERROR_NETWORK_ACCESS_DENIED:
    case ERROR_CANNOT_MAKE: case ERROR_FAIL_I24: case ERROR_DRIVE_LOCKED:
    case ERROR_SEEK_ON_DEVICE: case ERROR_NOT_LOCKED: case ERROR_LOCK_FAILED:
    case ERROR_DELETE_PENDING:
      return EACCES;
    case ERROR_PRIVILEGE_NOT_HELD: return EPERM;
    case ERROR_INVALID_HANDLE: case ERROR_INVALID_TARGET_HANDLE: case ERROR_DIRECT_ACCESS_HANDLE:
      return EBADF;
    case ERROR_ARENA_TRASHED: case ERROR_NOT_ENOUGH_MEMORY: case ERROR_INVALID_BLOCK:
    case ERROR_NOT_ENOUGH_QUOTA: case ERROR_COMMITMENT_LIMIT:
      return ENOMEM;
    case ERROR_BAD_ENVIRONMENT: return E2BIG;
    case ERROR_BAD_FORMAT: case ERROR_BAD_EXE_FORMAT: case ERROR_EXE_MARKED_INVALID:
      return ENOEXEC;
    case ERROR_NOT_SAME_DEVICE: return EXDEV;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return EEXIST;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_BROKEN_PIPE: case ERROR_NO_DATA: case ERROR_PIPE_NOT_CONNECTED: return EPIPE;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_NEGATIVE_SEEK: case ERROR_INVALID_PARAMETER: case ERROR_INVALID_FUNCTION:
      return EINVAL;
    case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    case ERROR_CANT_RESOLVE_FILENAME: return ELOOP;
    case ERROR_NOT_SUPPORTED: case ERROR_CALL_NOT_IMPLEMENTED: return ENOSYS;
    case ERROR_WAIT_NO_CHILDREN: case ERROR_CHILD_NOT_COMPLETE: return ECHILD;
    case ERROR_BUSY: case ERROR_PIPE_BUSY: return EBUSY;
    case ERROR_IO_PENDING: case ERROR_NO_PROC_SLOTS: return EAGAIN;
    case ERROR_OPERATION_ABORTED: return EINTR;
    case ERROR_NOT_A_REPARSE_POINT: return EINVAL;
    default: return EINVAL;
  }
}
static int sp_w32_fail(void) { errno = sp_w32_errno_of(GetLastError()); return -1; }

/* The errno POSIX would give a path Windows only says "not found" or
   "invalid" for: ENAMETOOLONG for a component past NAME_MAX, ENOTDIR when
   a component before the last is a file -- or when the path is one and a
   directory was wanted (chdir, opendir). */
static int sp_w32_path_errno(const wchar_t *w, int e, int want_dir) {
  if (!w || (e != ENOENT && e != EINVAL && e != ENOTDIR)) return e;
  /* a symlink that resolves to itself (or round a cycle) */
  {
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    else if (GetLastError() == ERROR_CANT_RESOLVE_FILENAME) return ELOOP;
  }
  if (e == ENOTDIR) return e;
  size_t comp = 0;
  for (const wchar_t *c = w; ; c++) {
    if (*c == 0 || *c == L'\\' || *c == L'/') { if (comp > 255) return ENAMETOOLONG; comp = 0; if (!*c) break; }
    else comp++;
  }
  size_t n = wcslen(w);
  wchar_t *buf = (wchar_t *)malloc(sizeof(wchar_t) * (n + 1));
  if (!buf) return e;
  wcscpy(buf, w);
  int r = e;
  for (size_t i = 1; i < n; i++) {
    if (buf[i] != L'\\' && buf[i] != L'/') continue;
    if (i == 2 && buf[1] == L':') continue;   /* a drive's root */
    wchar_t sv = buf[i]; buf[i] = 0;
    DWORD a = GetFileAttributesW(buf);
    buf[i] = sv;
    if (a == INVALID_FILE_ATTRIBUTES) break;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { r = ENOTDIR; break; }
  }
  if (r == e && want_dir) {
    DWORD a = GetFileAttributesW(w);
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) r = ENOTDIR;
  }
  free(buf);
  return r;
}

/* ===================================================================== */
/* UTF-8 <-> UTF-16 and the POSIX spellings of paths                      */
/* ===================================================================== */

#define SP_W32_WBUF 520

/* A path as Windows wants it: UTF-16, /dev/null as NUL. A caller passes a
   buffer; one too small for the path gets a malloc'd one (freed by
   sp_w32_wfree). NULL with errno set when the text is not UTF-8. */
/* A path past MAX_PATH reaches the file only in its \\?\ form: absolute,
   backslashed, no "." or "..". POSIX has no such edge (a tree a hundred
   levels deep is ordinary there), so a long one is rewritten that way. */
static wchar_t *sp_w32_long(wchar_t *w, wchar_t *buf) {
  if (wcslen(w) < 240 || !wcsncmp(w, L"\\\\?\\", 4)) return w;
  DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
  if (n == 0) return w;
  wchar_t *full = (wchar_t *)malloc(sizeof(wchar_t) * (n + 8));
  if (!full) return w;
  int unc = 0;
  wcscpy(full, L"\\\\?\\");
  DWORD k = GetFullPathNameW(w, n, full + 4, NULL);
  if (k == 0 || k >= n) { free(full); return w; }
  if (!wcsncmp(full + 4, L"\\\\", 2)) unc = 1;
  if (unc) {   /* \\server\share -> \\?\UNC\server\share */
    memmove(full + 8, full + 6, sizeof(wchar_t) * (wcslen(full + 6) + 1));
    memcpy(full + 4, L"UNC\\", sizeof(wchar_t) * 4);
  }
  if (w != buf) free(w);
  return full;
}

static wchar_t *sp_w32_wpath_short(const char *p, wchar_t *buf, size_t cap);
static wchar_t *sp_w32_wpath(const char *p, wchar_t *buf, size_t cap) {
  wchar_t *w = sp_w32_wpath_short(p, buf, cap);
  return w ? sp_w32_long(w, buf) : NULL;
}

static wchar_t *sp_w32_wpath_short(const char *p, wchar_t *buf, size_t cap) {
  if (!p) { errno = EFAULT; return NULL; }
  if (strcmp(p, "/dev/null") == 0) p = "NUL";
  /* /tmp is the system's temporary directory, as MSYS2 and Cygwin have it:
     the runtime and many programs name it outright */
  if (strncmp(p, "/tmp", 4) == 0 && (p[4] == 0 || p[4] == '/' || p[4] == '\\')) {
    wchar_t t[MAX_PATH + 1];
    DWORD tn = GetTempPathW(MAX_PATH + 1, t);
    if (tn > 0 && tn <= MAX_PATH) {
      while (tn > 0 && (t[tn - 1] == L'\\' || t[tn - 1] == L'/')) t[--tn] = 0;
      int rn = MultiByteToWideChar(CP_UTF8, 0, p + 4, -1, NULL, 0);
      if (rn <= 0) { errno = EILSEQ; return NULL; }
      size_t need = tn + (size_t)rn;
      wchar_t *w = need <= cap ? buf : (wchar_t *)malloc(sizeof(wchar_t) * need);
      if (!w) { errno = ENOMEM; return NULL; }
      memcpy(w, t, sizeof(wchar_t) * tn);
      MultiByteToWideChar(CP_UTF8, 0, p + 4, -1, w + tn, rn);
      return w;
    }
  }
  int n = MultiByteToWideChar(CP_UTF8, 0, p, -1, NULL, 0);
  if (n <= 0) { errno = EILSEQ; return NULL; }
  wchar_t *w = (size_t)n <= cap ? buf : (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n);
  if (!w) { errno = ENOMEM; return NULL; }
  MultiByteToWideChar(CP_UTF8, 0, p, -1, w, n);
  return w;
}
static void sp_w32_wfree(wchar_t *w, wchar_t *buf) { if (w && w != buf) free(w); }

/* UTF-16 to a malloc'd UTF-8 string */
static char *sp_w32_u8(const wchar_t *w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  if (n <= 0) return NULL;
  char *s = (char *)malloc((size_t)n);
  if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
  return s;
}

/* backslashes to slashes: what Ruby on Windows answers for a path it built */
static void sp_w32_slashes(char *s) { for (; *s; s++) if (*s == '\\') *s = '/'; }

/* A path as UTF-8 for a call that takes one but cannot go through the
   shim's wide-path calls (an AF_UNIX socket address): /tmp and /dev/null
   mapped the way sp_w32_wpath maps them. */
const char *sp_w32_path(const char *path, char *buf, size_t n) {
  wchar_t wb[SP_W32_WBUF];
  wchar_t *w = sp_w32_wpath(path, wb, SP_W32_WBUF);
  if (!w || !buf) { sp_w32_wfree(w, wb); return path; }
  if (WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)n, NULL, NULL) <= 0) snprintf(buf, n, "%s", path);
  sp_w32_wfree(w, wb);
  return buf;
}


/* ===================================================================== */
/* startup                                                               */
/* ===================================================================== */

/* the thread a process-directed signal is delivered to (see sp_w32_async),
   and Windows 11's QueueUserAPC2, which delivers it */
static HANDLE sp_w32_main_thread;
static DWORD sp_w32_main_tid;
typedef BOOL (WINAPI *sp_w32_qapc2_fn)(PAPCFUNC, HANDLE, ULONG_PTR, int);
static sp_w32_qapc2_fn sp_w32_qapc2;

static UINT sp_w32_saved_cp = 0, sp_w32_saved_icp = 0;
static void sp_w32_restore_cp(void) {
  if (sp_w32_saved_cp) SetConsoleOutputCP(sp_w32_saved_cp);
  if (sp_w32_saved_icp) SetConsoleCP(sp_w32_saved_icp);
}

/* Every stream binary, as on POSIX: a Ruby program's bytes are its bytes.
   The console reads and prints UTF-8 while the program runs (restored at
   exit, the console outlives it). Errors in a child must not pop a dialog box. */
/* a standard handle that is a console (a pipe, a file or NUL is not; the
   type check asks the console host nothing) */
static int sp_w32_std_console(DWORD which) {
  HANDLE h = GetStdHandle(which);
  DWORD mode;
  return h && h != INVALID_HANDLE_VALUE && GetFileType(h) == FILE_TYPE_CHAR && GetConsoleMode(h, &mode);
}

__attribute__((constructor)) static void sp_w32_boot(void) {
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sp_w32_main_thread,
                  0, FALSE, DUPLICATE_SAME_ACCESS);
  sp_w32_main_tid = GetCurrentThreadId();
  sp_w32_qapc2 = (sp_w32_qapc2_fn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueueUserAPC2");
  _fmode = _O_BINARY;
  _setmode(0, _O_BINARY);
  _setmode(1, _O_BINARY);
  _setmode(2, _O_BINARY);
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  /* TZ as a POSIX shell exports it (MSYS2's "Etc/UTC") is an IANA name the
     UCRT cannot read -- it takes the first three letters for the zone and
     invents the rest. UTC by any of its names is UTC0; another IANA name
     gives way to the system's own zone. */
  {
    const char *tz = getenv("TZ");
    if (tz && strchr(tz, '/')) {
      if (!_stricmp(tz, "Etc/UTC") || !_stricmp(tz, "Etc/GMT") || !_stricmp(tz, "Etc/Universal") ||
          !_stricmp(tz, "Etc/Zulu") || !_stricmp(tz, "UTC/UTC")) _putenv_s("TZ", "UTC0");
      else _putenv_s("TZ", "");
    } else if (tz && (!_stricmp(tz, "UTC") || !_stricmp(tz, "GMT") || !_stricmp(tz, "Z"))) _putenv_s("TZ", "UTC0");
    sp_w32_tzset();
  }
  /* HOME and USER as CRuby on Windows sets them at startup: HOME from
     %USERPROFILE% when it is unset, with forward slashes either way; USER
     from %USERNAME% */
  {
    const char *h = getenv("HOME");
    if (!h || !*h) h = getenv("USERPROFILE");
    if (h && *h) {
      char hb[MAX_PATH * 3];
      snprintf(hb, sizeof hb, "%s", h);
      sp_w32_slashes(hb);
      _putenv_s("HOME", hb);
    }
    const char *u = getenv("USER");
    if ((!u || !*u) && getenv("USERNAME")) _putenv_s("USER", getenv("USERNAME"));
  }
  /* The console's code pages to UTF-8, put back at exit -- only where a
     standard handle is a console. Under MSYS2's terminals, ssh and any
     redirection the handles are pipes or files: the bytes reach their
     reader as they are, and the console calls (each a round trip to the
     console host) were a fifth of a small program's start. */
  if (sp_w32_std_console(STD_OUTPUT_HANDLE) || sp_w32_std_console(STD_ERROR_HANDLE)) {
    UINT cp = GetConsoleOutputCP();
    if (cp && cp != CP_UTF8 && SetConsoleOutputCP(CP_UTF8)) { sp_w32_saved_cp = cp; atexit(sp_w32_restore_cp); }
  }
  if (sp_w32_std_console(STD_INPUT_HANDLE)) {
    UINT icp = GetConsoleCP();
    if (icp && icp != CP_UTF8 && SetConsoleCP(CP_UTF8)) { sp_w32_saved_icp = icp; if (!sp_w32_saved_cp) atexit(sp_w32_restore_cp); }
  }
}

/* ===================================================================== */
/* files and paths                                                       */
/* ===================================================================== */

static int sp_w32_is_dir_w(const wchar_t *w) {
  DWORD a = GetFileAttributesW(w);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int sp_w32_open(const char *path, int flags, ...) {
  int mode = 0666;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
  wchar_t wb[SP_W32_WBUF], *w = sp_w32_wpath(path, wb, SP_W32_WBUF);
  if (!w) return -1;
  DWORD access = 0;
  switch (flags & O_ACCMODE) {
    case O_RDONLY: access = GENERIC_READ; break;
    case O_WRONLY: access = GENERIC_WRITE; break;
    default:       access = GENERIC_READ | GENERIC_WRITE; break;
  }
  DWORD disp;
  if ((flags & O_CREAT) && (flags & O_EXCL)) disp = CREATE_NEW;
  else if ((flags & O_CREAT) && (flags & O_TRUNC)) disp = CREATE_ALWAYS;
  else if (flags & O_CREAT) disp = OPEN_ALWAYS;
  else if (flags & O_TRUNC) disp = TRUNCATE_EXISTING;
  else disp = OPEN_EXISTING;
  DWORD attrs = FILE_ATTRIBUTE_NORMAL;
  if ((flags & O_CREAT) && !(mode & 0200)) attrs = FILE_ATTRIBUTE_READONLY;
  DWORD fl = attrs;
  int is_dir = sp_w32_is_dir_w(w);
  if (is_dir) {
    /* POSIX opens a directory read-only (and refuses it for writing) */
    if ((flags & O_ACCMODE) != O_RDONLY) { sp_w32_wfree(w, wb); errno = EISDIR; return -1; }
    fl |= FILE_FLAG_BACKUP_SEMANTICS;
  } else if (flags & O_DIRECTORY) {
    errno = GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES ? ENOENT : ENOTDIR;
    sp_w32_wfree(w, wb);
    return -1;
  }
  SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, FALSE };
  HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         &sa, disp, fl, NULL);
  DWORD err = GetLastError();
  if (h == INVALID_HANDLE_VALUE) { errno = sp_w32_path_errno(w, sp_w32_errno_of(err), 0); sp_w32_wfree(w, wb); return -1; }
  sp_w32_wfree(w, wb);
  /* TRUNCATE_EXISTING needs write access, which O_RDONLY|O_TRUNC lacks --
     POSIX leaves that combination undefined; the file is left as it is */
  int cf = _O_BINARY | (flags & O_APPEND ? _O_APPEND : 0) |
           ((flags & O_ACCMODE) == O_RDONLY ? _O_RDONLY : 0);
  int fd = _open_osfhandle((intptr_t)h, cf);
  if (fd < 0) { CloseHandle(h); errno = EMFILE; return -1; }
  return fd;
}

int sp_w32_mkdir(const char *path, int mode) {
  (void)mode;
  wchar_t wb[SP_W32_WBUF], *w = sp_w32_wpath(path, wb, SP_W32_WBUF);
  if (!w) return -1;
  int r = _wmkdir(w);
  if (r != 0) errno = sp_w32_path_errno(w, errno, 0);
  sp_w32_wfree(w, wb);
  return r;
}

int sp_w32_chdir(const char *path) {
  wchar_t wb[SP_W32_WBUF], *w = sp_w32_wpath(path, wb, SP_W32_WBUF);
  if (!w) return -1;
  int r = 0;
  if (!SetCurrentDirectoryW(w)) {
    DWORD e = GetLastError();
    errno = sp_w32_path_errno(w, e == ERROR_DIRECTORY ? ENOTDIR : sp_w32_errno_of(e), 1);
    r = -1;
  }
  sp_w32_wfree(w, wb);
  return r;
}

char *sp_w32_getcwd(char *buf, size_t n) {
  wchar_t *w = _wgetcwd(NULL, 0);
  if (!w) return NULL;
  char *u = sp_w32_u8(w);
  free(w);
  if (!u) { errno = EILSEQ; return NULL; }
  sp_w32_slashes(u);
  if (!buf) return u;
  if (strlen(u) + 1 > n) { free(u); errno = ERANGE; return NULL; }
  strcpy(buf, u);
  free(u);
  return buf;
}

int sp_w32_access(const char *path, int mode) {
  wchar_t wb[SP_W32_WBUF], *w = sp_w32_wpath(path, wb, SP_W32_WBUF);
  if (!w) return -1;
  DWORD a = GetFileAttributesW(w);
  if (a == INVALID_FILE_ATTRIBUTES) { errno = sp_w32_path_errno(w, sp_w32_errno_of(GetLastError()), 0); sp_w32_wfree(w, wb); return -1; }
  sp_w32_wfree(w, wb);
  if ((mode & W_OK) && (a & FILE_ATTRIBUTE_READONLY) && !(a & FILE_ATTRIBUTE_DIRECTORY)) { errno = EACCES; return -1; }
  if ((mode & X_OK) && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
    /* executable means a program Windows would run */
    const char *dot = strrchr(path, '.');
    const char *sl = strrchr(path, '/'), *bs = strrchr(path, '\\');
    if (bs > sl) sl = bs;
    if (!dot || (sl && dot < sl) ||
        (_stricmp(dot, ".exe") && _stricmp(dot, ".com") && _stricmp(dot, ".bat") && _stricmp(dot, ".cmd"))) {
      errno = EACCES; return -1;
    }
  }
  return 0;
}

int sp_w32_unlink(const char *path) {
  wchar_t wb[SP_W32_WBUF], *w = sp_w32_wpath(path, wb, SP_W32_WBUF);
  if (!w) return -1;
  int r = 0;
  DWORD a = GetFileAttributesW(w);
  if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
    errno = EISDIR; r = -1;   /* POSIX unlink refuses a directory (Linux: EISDIR) */
  } else if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
    if (!RemoveDirectoryW(w)) r = sp_w32_fail();   /* a directory symlink */
  } else if (!DeleteFileW(w)) {
    DWORD e = GetLastError();
    /* POSIX unlinks a read-only file; Windows wants the attribute off first */
    if (e == ERROR_ACCESS_DENIED && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY) &&
        SetFileAttributesW(w, a & ~FILE_ATTRIBUTE_READONLY)) {
      if (!DeleteFileW(w)) { e = GetLastError(); SetFileAttributesW(w, a); errno = sp_w32_errno_of(e); r = -1; }
    } else { errno = sp_w32_path_errno(w, sp_w32_errno_of(e), 0); r = -1; }
  }
  sp_w32_wfree(w, wb);
  return r;
}

int sp_w32_rmdir(const char *path) {
  wchar_t wb[SP_W32_WBUF], *w = sp_w32_wpath(path, wb, SP_W32_WBUF);
  if (!w) return -1;
  int r = 0;
  if (!RemoveDirectoryW(w)) {
    DWORD e = GetLastError();
    errno = e == ERROR_DIRECTORY ? ENOTDIR : e == ERROR_DIR_NOT_EMPTY ? ENOTEMPTY : sp_w32_path_errno(w, sp_w32_errno_of(e), 1);
    r = -1;
  }
  sp_w32_wfree(w, wb);
  return r;
}

int sp_w32_truncate(const char *path, long long len) {
  wchar_t wb[SP_W32_WBUF], *w = sp_w32_wpath(path, wb, SP_W32_WBUF);
  if (!w) return -1;
  HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  DWORD err = GetLastError();
  sp_w32_wfree(w, wb);
  if (h == INVALID_HANDLE_VALUE) { errno = sp_w32_errno_of(err); return -1; }
  FILE_END_OF_FILE_INFO eof; eof.EndOfFile.QuadPart = len;
  BOOL ok = SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof eof);
  err = GetLastError();
  CloseHandle(h);
  if (!ok) { errno = sp_w32_errno_of(err); return -1; }
  return 0;
}

int sp_w32_remove(const char *path) {
  wchar_t wb[SP_W32_WBUF], *w = sp_w32_wpath(path, wb, SP_W32_WBUF);
  if (!w) return -1;
  int dir = sp_w32_is_dir_w(w);
  int r = 0;
  if (dir) { if (!RemoveDirectoryW(w)) r = sp_w32_fail(); }
  sp_w32_wfree(w, wb);
  return dir ? r : sp_w32_unlink(path);
}

int sp_w32_rename(const char *from, const char *to) {
  wchar_t fb[SP_W32_WBUF], tb[SP_W32_WBUF];
  wchar_t *wf = sp_w32_wpath(from, fb, SP_W32_WBUF);
  if (!wf) return -1;
  wchar_t *wt = sp_w32_wpath(to, tb, SP_W32_WBUF);
  if (!wt) { sp_w32_wfree(wf, fb); return -1; }
  int r = 0;
  /* POSIX rename replaces the target, file over file or onto an empty
     directory */
  if (!MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
    DWORD e = GetLastError();
    if (e == ERROR_ACCESS_DENIED && sp_w32_is_dir_w(wt) && sp_w32_is_dir_w(wf) &&
        RemoveDirectoryW(wt) && MoveFileExW(wf, wt, 0)) r = 0;
    else { errno = sp_w32_errno_of(e); r = -1; }
  }
  sp_w32_wfree(wf, fb); sp_w32_wfree(wt, tb);
  return r;
}

int sp_w32_link(const char *target, const char *path) {
  wchar_t tb[SP_W32_WBUF], pb[SP_W32_WBUF];
  wchar_t *wt = sp_w32_wpath(target, tb, SP_W32_WBUF);
  if (!wt) return -1;
  wchar_t *wp = sp_w32_wpath(path, pb, SP_W32_WBUF);
  if (!wp) { sp_w32_wfree(wt, tb); return -1; }
  int r = CreateHardLinkW(wp, wt, NULL) ? 0 : sp_w32_fail();
  sp_w32_wfree(wt, tb); sp_w32_wfree(wp, pb);
  return r;
}

int sp_w32_symlink(const char *target, const char *path) {
  wchar_t tb[SP_W32_WBUF], pb[SP_W32_WBUF];
  /* the link's text is stored as given, slashes turned the Windows way --
     except a /tmp target, which names the temporary directory here and has
     to say so to resolve (readlink then answers the directory's own path) */
  char *t = strdup(target ? target : "");
  if (!t) { errno = ENOMEM; return -1; }
  if (!(strncmp(t, "/tmp", 4) == 0 && (t[4] == 0 || t[4] == '/')))
    for (char *c = t; *c; c++) if (*c == '/') *c = '\\';
  wchar_t *wt = sp_w32_wpath(t, tb, SP_W32_WBUF);
  free(t);
  if (!wt) return -1;
  for (wchar_t *c = wt; *c; c++) if (*c == L'/') *c = L'\\';
  wchar_t *wp = sp_w32_wpath(path, pb, SP_W32_WBUF);
  if (!wp) { sp_w32_wfree(wt, tb); return -1; }
  /* a relative target is relative to the link's directory */
  DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
  {
    wchar_t full[MAX_PATH * 2];
    const wchar_t *probe = wt;
    if (!(wt[0] == L'\\' || (wt[0] && wt[1] == L':'))) {
      wcsncpy(full, wp, MAX_PATH); full[MAX_PATH] = 0;
      wchar_t *sep = wcsrchr(full, L'\\'), *sep2 = wcsrchr(full, L'/');
      if (sep2 > sep) sep = sep2;
      if (sep) { sep[1] = 0; wcsncat(full, wt, MAX_PATH - 1); probe = full; }
    }
    if (sp_w32_is_dir_w(probe)) flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
  }
  int r = 0;
  if (GetFileAttributesW(wp) != INVALID_FILE_ATTRIBUTES) { errno = EEXIST; r = -1; }
  else if (!CreateSymbolicLinkW(wp, wt, flags)) {
    DWORD e = GetLastError();
    if (e == ERROR_INVALID_PARAMETER) {   /* before 1703: no unprivileged flag */
      if (!CreateSymbolicLinkW(wp, wt, flags & ~SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) r = sp_w32_fail();
    } else { errno = e == ERROR_PRIVILEGE_NOT_HELD ? EPERM : sp_w32_errno_of(e); r = -1; }
  }
  sp_w32_wfree(wt, tb); sp_w32_wfree(wp, pb);
  return r;
}

/* the reparse data a symlink or a junction carries */
typedef struct {
  ULONG ReparseTag; USHORT ReparseDataLength; USHORT Reserved;
  union {
    struct { USHORT SubstituteNameOffset, SubstituteNameLength, PrintNameOffset, PrintNameLength; ULONG Flags; WCHAR PathBuffer[1]; } Sym;
    struct { USHORT SubstituteNameOffset, SubstituteNameLength, PrintNameOffset, PrintNameLength; WCHAR PathBuffer[1]; } Mnt;
  } u;
} sp_w32_reparse;

long long sp_w32_readlink(const char *path, char *buf, size_t n) {
  /* the driver asks where it was run from the Linux way */
  if (path && strcmp(path, "/proc/self/exe") == 0) {
    wchar_t w[MAX_PATH * 4];
    DWORD k = GetModuleFileNameW(NULL, w, (DWORD)(sizeof w / sizeof w[0]));
    if (k == 0 || k >= sizeof w / sizeof w[0]) return sp_w32_fail();
    char *u = sp_w32_u8(w);
    if (!u) { errno = EILSEQ; return -1; }
    sp_w32_slashes(u);
    size_t len = strlen(u);
    if (len > n) len = n;
    memcpy(buf, u, len);
    free(u);
    return (long long)len;
  }
  wchar_t wb[SP_W32_WBUF], *w = sp_w32_wpath(path, wb, SP_W32_WBUF);
  if (!w) return -1;
  HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
  sp_w32_wfree(w, wb);
  if (h == INVALID_HANDLE_VALUE) return sp_w32_fail();
  union { sp_w32_reparse r; char raw[MAXIMUM_REPARSE_DATA_BUFFER_SIZE]; } rb;
  DWORD got = 0;
  BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, &rb, sizeof rb, &got, NULL);
  DWORD err = GetLastError();
  CloseHandle(h);
  if (!ok) { errno = err == ERROR_NOT_A_REPARSE_POINT ? EINVAL : sp_w32_errno_of(err); return -1; }
  const WCHAR *name; USHORT nlen;
  if (rb.r.ReparseTag == IO_REPARSE_TAG_SYMLINK) {
    name = rb.r.u.Sym.PathBuffer + rb.r.u.Sym.PrintNameOffset / sizeof(WCHAR);
    nlen = rb.r.u.Sym.PrintNameLength / sizeof(WCHAR);
    if (nlen == 0) {
      name = rb.r.u.Sym.PathBuffer + rb.r.u.Sym.SubstituteNameOffset / sizeof(WCHAR);
      nlen = rb.r.u.Sym.SubstituteNameLength / sizeof(WCHAR);
    }
  } else if (rb.r.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
    name = rb.r.u.Mnt.PathBuffer + rb.r.u.Mnt.PrintNameOffset / sizeof(WCHAR);
    nlen = rb.r.u.Mnt.PrintNameLength / sizeof(WCHAR);
  } else { errno = EINVAL; return -1; }
  wchar_t tmp[MAX_PATH * 4];
  if (nlen >= sizeof tmp / sizeof tmp[0]) nlen = (USHORT)(sizeof tmp / sizeof tmp[0] - 1);
  memcpy(tmp, name, nlen * sizeof(WCHAR)); tmp[nlen] = 0;
  wchar_t *t = tmp;
  if (wcsncmp(t, L"\\??\\", 4) == 0) t += 4;
  char *u = sp_w32_u8(t);
  if (!u) { errno = EILSEQ; return -1; }
  sp_w32_slashes(u);
  size_t len = strlen(u);
  if (len > n) len = n;
  memcpy(buf, u, len);
  free(u);
  return (long long)len;
}

char *sp_w32_realpath(const char *path, char *resolved) {
  wchar_t wb[SP_W32_WBUF], *w = sp_w32_wpath(path, wb, SP_W32_WBUF);
  if (!w) return NULL;
  HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  DWORD err = GetLastError();
  sp_w32_wfree(w, wb);
  if (h == INVALID_HANDLE_VALUE) { errno = sp_w32_errno_of(err); return NULL; }
  wchar_t fin[MAX_PATH * 4];
  DWORD k = GetFinalPathNameByHandleW(h, fin, (DWORD)(sizeof fin / sizeof fin[0]), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  CloseHandle(h);
  if (k == 0 || k >= sizeof fin / sizeof fin[0]) { errno = ENAMETOOLONG; return NULL; }
  wchar_t *f = fin;
  if (wcsncmp(f, L"\\\\?\\UNC\\", 8) == 0) { f += 6; f[0] = L'\\'; }
  else if (wcsncmp(f, L"\\\\?\\", 4) == 0) f += 4;
  char *u = sp_w32_u8(f);
  if (!u) { errno = EILSEQ; return NULL; }
  sp_w32_slashes(u);
  if (!resolved) return u;
  snprintf(resolved, MAX_PATH, "%s", u);
  free(u);
  return resolved;
}

/* ---- stat ---- */

static void sp_w32_ft2ts(const FILETIME *ft, struct timespec *ts) {
  ULARGE_INTEGER u; u.LowPart = ft->dwLowDateTime; u.HighPart = ft->dwHighDateTime;
  long long t = (long long)(u.QuadPart - 116444736000000000ULL);
  long long s = t / 10000000LL, r = t % 10000000LL;
  if (r < 0) { r += 10000000LL; s--; }
  ts->tv_sec = (time_t)s;
  ts->tv_nsec = (long)(r * 100);
}
static void sp_w32_li2ts(LARGE_INTEGER li, struct timespec *ts) {
  FILETIME ft; ft.dwLowDateTime = (DWORD)li.LowPart; ft.dwHighDateTime = (DWORD)li.HighPart;
  sp_w32_ft2ts(&ft, ts);
}

static int sp_w32_exe_name(const wchar_t *w) {
  const wchar_t *dot = wcsrchr(w, L'.');
  const wchar_t *sl = wcsrchr(w, L'\\'), *sl2 = wcsrchr(w, L'/');
  if (sl2 > sl) sl = sl2;
  if (!dot || (sl && dot < sl)) return 0;
  return !_wcsicmp(dot, L".exe") || !_wcsicmp(dot, L".com") || !_wcsicmp(dot, L".bat") || !_wcsicmp(dot, L".cmd");
}

static int sp_w32_stat_handle(HANDLE h, const wchar_t *name, int nofollow, struct sp_w32_stat *st) {
  memset(st, 0, sizeof *st);
  DWORD ft = GetFileType(h);
  if (ft == FILE_TYPE_CHAR) { st->st_mode = S_IFCHR | 0666; st->st_nlink = 1; st->st_blksize = 4096; return 0; }
  if (ft == FILE_TYPE_PIPE) {
    st->st_mode = (sp_w32_is_socket_handle(h) ? S_IFSOCK : _S_IFIFO) | 0600;
    st->st_nlink = 1; st->st_blksize = 4096;
    DWORD avail = 0;
    if (PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) st->st_size = avail;
    return 0;
  }
  BY_HANDLE_FILE_INFORMATION bi;
  if (!GetFileInformationByHandle(h, &bi)) return sp_w32_fail();
  FILE_BASIC_INFO basic;
  int have_basic = GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof basic);
  FILE_STANDARD_INFO std;
  int have_std = GetFileInformationByHandleEx(h, FileStandardInfo, &std, sizeof std);
  DWORD tag = 0;
  if (bi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    FILE_ATTRIBUTE_TAG_INFO ti;
    if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &ti, sizeof ti)) tag = ti.ReparseTag;
  }
  unsigned mode;
  if (nofollow && (tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT)) mode = S_IFLNK | 0777;
  else if (tag == IO_REPARSE_TAG_AF_UNIX) mode = S_IFSOCK | 0755;
  else if (bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) mode = S_IFDIR | 0755;
  else {
    mode = S_IFREG | ((bi.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644);
    if (name && sp_w32_exe_name(name)) mode |= 0111;
  }
  st->st_mode = mode;
  st->st_dev = bi.dwVolumeSerialNumber;
  st->st_ino = ((unsigned long long)bi.nFileIndexHigh << 32) | bi.nFileIndexLow;
  st->st_nlink = bi.nNumberOfLinks;
  st->st_size = (long long)(((unsigned long long)bi.nFileSizeHigh << 32) | bi.nFileSizeLow);
  if ((mode & S_IFMT) == S_IFDIR) st->st_size = 0;
  st->st_blksize = 4096;
  st->st_blocks = have_std ? (long long)((std.AllocationSize.QuadPart + 511) / 512) : (st->st_size + 511) / 512;
  sp_w32_ft2ts(&bi.ftLastAccessTime, &st->st_atim);
  sp_w32_ft2ts(&bi.ftLastWriteTime, &st->st_mtim);
  sp_w32_ft2ts(&bi.ftCreationTime, &st->st_birthtim);
  if (have_basic) sp_w32_li2ts(basic.ChangeTime, &st->st_ctim);
  else st->st_ctim = st->st_mtim;
  return 0;
}

static int sp_w32_stat_path(const char *path, struct sp_w32_stat *st, int nofollow) {
  if (!path) { errno = EFAULT; return -1; }
  if (!*path) { errno = ENOENT; return -1; }
  wchar_t wb[SP_W32_WBUF], *w = sp_w32_wpath(path, wb, SP_W32_WBUF);
  if (!w) return -1;
  /* "x/" names a directory; Windows opens the file anyway */
  size_t pl = strlen(path);
  int want_dir = pl > 1 && (path[pl - 1] == '/' || path[pl - 1] == '\\');
  DWORD fl = FILE_FLAG_BACKUP_SEMANTICS | (nofollow ? FILE_FLAG_OPEN_REPARSE_POINT : 0);
  HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, fl, NULL);
  int r;
  if (h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    /* a file no one may open (pagefile.sys): the directory listing knows it */
    WIN32_FIND_DATAW fd;
    HANDLE fh = (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED) ? FindFirstFileW(w, &fd) : INVALID_HANDLE_VALUE;
    if (fh == INVALID_HANDLE_VALUE) { errno = sp_w32_path_errno(w, sp_w32_errno_of(e), 0); sp_w32_wfree(w, wb); return -1; }
    FindClose(fh);
    memset(st, 0, sizeof *st);
    st->st_mode = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? S_IFDIR | 0755 :
                  S_IFREG | ((fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644);
    st->st_nlink = 1;
    st->st_size = (long long)(((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
    st->st_blksize = 4096; st->st_blocks = (st->st_size + 511) / 512;
    sp_w32_ft2ts(&fd.ftLastAccessTime, &st->st_atim);
    sp_w32_ft2ts(&fd.ftLastWriteTime, &st->st_mtim);
    sp_w32_ft2ts(&fd.ftCreationTime, &st->st_birthtim);
    st->st_ctim = st->st_mtim;
    r = 0;
  } else {
    r = sp_w32_stat_handle(h, w, nofollow, st);
    CloseHandle(h);
  }
  sp_w32_wfree(w, wb);
  if (r == 0 && want_dir && !S_ISDIR(st->st_mode)) { errno = ENOTDIR; return -1; }
  return r;
}

int sp_w32_stat(const char *path, struct sp_w32_stat *st) { return sp_w32_stat_path(path, st, 0); }
int sp_w32_lstat(const char *path, struct sp_w32_stat *st) { return sp_w32_stat_path(path, st, 1); }
int sp_w32_fstat(int fd, struct sp_w32_stat *st) {
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
  return sp_w32_stat_handle(h, NULL, 0, st);
}

int sp_w32_chmod(const char *path, int mode) {
  wchar_t wb[SP_W32_WBUF], *w = sp_w32_wpath(path, wb, SP_W32_WBUF);
  if (!w) return -1;
  int r = 0;
  DWORD a = GetFileAttributesW(w);
  if (a == INVALID_FILE_ATTRIBUTES) r = sp_w32_fail();
  else {
    DWORD na = (mode & 0200) ? (a & ~FILE_ATTRIBUTE_READONLY) : (a | FILE_ATTRIBUTE_READONLY);
    if (a & FILE_ATTRIBUTE_DIRECTORY) na = a;   /* a directory's read-only bit means something else */
    if (na != a && !SetFileAttributesW(w, na)) r = sp_w32_fail();
  }
  sp_w32_wfree(w, wb);
  return r;
}

int fchmod(int fd, mode_t mode) {
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
  FILE_BASIC_INFO bi;
  if (!GetFileInformationByHandleEx(h, FileBasicInfo, &bi, sizeof bi)) return sp_w32_fail();
  DWORD a = bi.FileAttributes;
  DWORD na = (mode & 0200) ? (a & ~FILE_ATTRIBUTE_READONLY) : (a | FILE_ATTRIBUTE_READONLY);
  if (a & FILE_ATTRIBUTE_DIRECTORY) return 0;
  if (na == a) return 0;
  bi.FileAttributes = na ? na : FILE_ATTRIBUTE_NORMAL;
  return SetFileInformationByHandle(h, FileBasicInfo, &bi, sizeof bi) ? 0 : sp_w32_fail();
}

int mkfifo(const char *path, mode_t mode) { (void)path; (void)mode; errno = ENOSYS; return -1; }

/* utimensat(AT_FDCWD, path, times, flags): the access and write times */
int sp_w32_utimensat(int dirfd, const char *path, const struct timespec ts[2], int flags) {
  (void)dirfd;
  wchar_t wb[SP_W32_WBUF], *w = sp_w32_wpath(path, wb, SP_W32_WBUF);
  if (!w) return -1;
  HANDLE h = CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
                         ((flags & AT_SYMLINK_NOFOLLOW) ? FILE_FLAG_OPEN_REPARSE_POINT : 0), NULL);
  DWORD err = GetLastError();
  sp_w32_wfree(w, wb);
  if (h == INVALID_HANDLE_VALUE) { errno = sp_w32_errno_of(err); return -1; }
  FILETIME ft[2], *fp[2] = { NULL, NULL };
  FILETIME now; GetSystemTimeAsFileTime(&now);
  for (int i = 0; i < 2; i++) {
    if (ts && ts[i].tv_nsec == UTIME_OMIT) continue;
    if (!ts || ts[i].tv_nsec == UTIME_NOW) { ft[i] = now; fp[i] = &ft[i]; continue; }
    ULARGE_INTEGER u;
    u.QuadPart = (unsigned long long)((long long)ts[i].tv_sec * 10000000LL + ts[i].tv_nsec / 100) + 116444736000000000ULL;
    ft[i].dwLowDateTime = u.LowPart; ft[i].dwHighDateTime = u.HighPart; fp[i] = &ft[i];
  }
  BOOL ok = SetFileTime(h, NULL, fp[0], fp[1]);
  err = GetLastError();
  CloseHandle(h);
  if (!ok) { errno = sp_w32_errno_of(err); return -1; }
  return 0;
}

int sp_w32_utimes(const char *path, const struct timeval tv[2]) {
  struct timespec ts[2];
  if (tv) for (int i = 0; i < 2; i++) { ts[i].tv_sec = tv[i].tv_sec; ts[i].tv_nsec = tv[i].tv_usec * 1000; }
  return sp_w32_utimensat(AT_FDCWD, path, tv ? ts : NULL, 0);
}

int sp_w32_faccessat(int dirfd, const char *path, int mode, int flags) {
  (void)dirfd; (void)flags;
  return sp_w32_access(path, mode);
}

/* ---- flock: LockFileEx far past any data, so it locks out other flocks
   and never another process's reads and writes (Windows locks are
   mandatory; flock's are advisory) ---- */
int flock(int fd, int op) {
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
  /* exactly one of shared, exclusive and unlock, as Linux checks */
  int kind = op & (LOCK_SH | LOCK_EX | LOCK_UN);
  if (kind != LOCK_SH && kind != LOCK_EX && kind != LOCK_UN) { errno = EINVAL; return -1; }
  OVERLAPPED ov; memset(&ov, 0, sizeof ov);
  ov.Offset = 0xFFFFFFFE; ov.OffsetHigh = 0x7FFFFFFF;
  if (op & LOCK_UN) {
    if (!UnlockFileEx(h, 0, 1, 0, &ov)) { if (GetLastError() == ERROR_NOT_LOCKED) return 0; return sp_w32_fail(); }
    return 0;
  }
  DWORD fl = 0;
  if (op & LOCK_EX) fl |= LOCKFILE_EXCLUSIVE_LOCK;
  if (op & LOCK_NB) fl |= LOCKFILE_FAIL_IMMEDIATELY;
  /* converting a held lock: drop it first, as flock does */
  UnlockFileEx(h, 0, 1, 0, &ov);
  if (!LockFileEx(h, fl, 0, 1, 0, &ov)) {
    DWORD e = GetLastError();
    if (e == ERROR_LOCK_VIOLATION || e == ERROR_IO_PENDING) { errno = EWOULDBLOCK; return -1; }
    errno = sp_w32_errno_of(e); return -1;
  }
  return 0;
}

int fsync(int fd) { return _commit(fd); }
int fdatasync(int fd) { return _commit(fd); }

/* pread/pwrite: at an offset, leaving the file position where it was */
long long pread(int fd, void *buf, size_t n, long long off) {
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
  LARGE_INTEGER zero = {0}, cur;
  if (!SetFilePointerEx(h, zero, &cur, FILE_CURRENT)) return sp_w32_fail();
  OVERLAPPED ov; memset(&ov, 0, sizeof ov);
  ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)((unsigned long long)off >> 32);
  DWORD got = 0;
  BOOL ok = ReadFile(h, buf, (DWORD)(n > 0x7fffffff ? 0x7fffffff : n), &got, &ov);
  DWORD e = GetLastError();
  SetFilePointerEx(h, cur, NULL, FILE_BEGIN);
  if (!ok && e != ERROR_HANDLE_EOF) { errno = sp_w32_errno_of(e); return -1; }
  return got;
}
long long pwrite(int fd, const void *buf, size_t n, long long off) {
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
  LARGE_INTEGER zero = {0}, cur;
  if (!SetFilePointerEx(h, zero, &cur, FILE_CURRENT)) return sp_w32_fail();
  OVERLAPPED ov; memset(&ov, 0, sizeof ov);
  ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)((unsigned long long)off >> 32);
  DWORD put = 0;
  BOOL ok = WriteFile(h, buf, (DWORD)(n > 0x7fffffff ? 0x7fffffff : n), &put, &ov);
  DWORD e = GetLastError();
  SetFilePointerEx(h, cur, NULL, FILE_BEGIN);
  if (!ok) { errno = sp_w32_errno_of(e); return -1; }
  return put;
}

/* ---- stdio over the shim's open ---- */

static int sp_w32_mode_flags(const char *m, int *oflags) {
  int f, plus = strchr(m, '+') != NULL;
  switch (m[0]) {
    case 'r': f = plus ? O_RDWR : O_RDONLY; break;
    case 'w': f = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    case 'a': f = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
    default: errno = EINVAL; return -1;
  }
  if (strchr(m, 'x')) f |= O_EXCL;
  if (strchr(m, 'e')) f |= O_CLOEXEC;
  *oflags = f;
  return 0;
}

/* the CRT's mode letters, binary, without the POSIX-only ones */
static void sp_w32_crt_mode(const char *m, char *out) {
  int k = 0;
  for (; *m && k < 6; m++) if (*m == 'r' || *m == 'w' || *m == 'a' || *m == '+') out[k++] = *m;
  out[k++] = 'b';
  out[k] = 0;
}

FILE *sp_w32_fopen(const char *path, const char *mode) {
  int of;
  if (!mode || sp_w32_mode_flags(mode, &of) != 0) { errno = EINVAL; return NULL; }
  int fd = sp_w32_open(path, of, 0666);
  if (fd < 0) return NULL;
  char cm[8]; sp_w32_crt_mode(mode, cm);
  FILE *f = _fdopen(fd, cm);
  if (!f) { int e = errno; _close(fd); errno = e; }
  return f;
}

FILE *sp_w32_freopen(const char *path, const char *mode, FILE *f) {
  if (!path) { errno = EINVAL; return NULL; }   /* the mode-only form: Windows cannot */
  int of;
  if (!mode || sp_w32_mode_flags(mode, &of) != 0) { errno = EINVAL; return NULL; }
  int fd = sp_w32_open(path, of, 0666);
  if (fd < 0) return NULL;
  int target = _fileno(f);
  fflush(f);
  if (_dup2(fd, target) != 0) { int e = errno; _close(fd); errno = e; return NULL; }
  _close(fd);
  clearerr(f);
  rewind(f);
  if (of & O_APPEND) fseek(f, 0, SEEK_END);
  return f;
}

#undef fflush
/* the descriptor back at the stream's position, its read-ahead dropped:
   an absolute seek (the UCRT takes a seek by 0 from SEEK_CUR as nothing to
   do), and the buffer emptied by hand if that left it */
static void sp_w32_resync(FILE *f) {
  long long pos = _ftelli64(f);
  if (pos < 0) return;
  if (_fseeki64(f, pos, SEEK_SET) == 0 && __freadahead(f) == 0) return;
  __fpurge(f);
  _lseeki64(_fileno(f), pos, SEEK_SET);
}

int sp_w32_fflush(FILE *f) {
  if (f && __freadahead(f) > 0) { sp_w32_resync(f); return 0; }
  return fflush(f);
}

#undef setvbuf
int sp_w32_setvbuf(FILE *f, char *buf, int mode, size_t size) {
  /* a read buffer's bytes go back to the descriptor, a write buffer is
     written (the UCRT's fflush on a read stream would DROP what it holds) */
  if (f) { if (__freadahead(f) > 0) sp_w32_resync(f); else fflush(f); }
  return setvbuf(f, buf, mode, size);
}

FILE *sp_w32_fdopen(int fd, const char *mode) {
  char cm[8]; sp_w32_crt_mode(mode ? mode : "r", cm);
  return _fdopen(fd, cm);
}

/* getdelim(3): the line including the delimiter, the buffer grown as needed */
long long getdelim(char **line, size_t *cap, int delim, FILE *f) {
  if (!line || !cap || !f) { errno = EINVAL; return -1; }
  if (!*line || *cap == 0) { *cap = 128; *line = (char *)malloc(*cap); if (!*line) { errno = ENOMEM; return -1; } }
  size_t n = 0;
  _lock_file(f);
  for (;;) {
    int c = _getc_nolock(f);
    if (c == EOF) break;
    if (n + 2 > *cap) {
      size_t nc = *cap * 2;
      char *nl = (char *)realloc(*line, nc);
      if (!nl) { _unlock_file(f); errno = ENOMEM; return -1; }
      *line = nl; *cap = nc;
    }
    (*line)[n++] = (char)c;
    if (c == delim) break;
  }
  _unlock_file(f);
  (*line)[n] = 0;
  return n ? (long long)n : -1;
}
long long getline(char **line, size_t *cap, FILE *f) { return getdelim(line, cap, '\n', f); }

int vdprintf(int fd, const char *fmt, va_list ap) {
  char sb[1024];
  va_list aq; va_copy(aq, ap);
  int n = vsnprintf(sb, sizeof sb, fmt, aq);
  va_end(aq);
  if (n < 0) return -1;
  char *b = sb;
  if ((size_t)n >= sizeof sb) {
    b = (char *)malloc((size_t)n + 1);
    if (!b) { errno = ENOMEM; return -1; }
    vsnprintf(b, (size_t)n + 1, fmt, ap);
  }
  long long w = sp_w32_write(fd, b, (size_t)n);
  if (b != sb) free(b);
  return w < 0 ? -1 : (int)w;
}
int dprintf(int fd, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int r = vdprintf(fd, fmt, ap);
  va_end(ap);
  return r;
}

/* the UCRT's stream internals, exported for exactly this */
_CRTIMP errno_t __cdecl _get_stream_buffer_pointers(FILE *, char ***, char ***, int **);
/* Whether a stream is reading. The UCRT's count is what is left in the
   buffer either way -- unread bytes on a read, free space on a write -- so
   only the stream's _IOREAD flag tells them apart. The UCRT's FILE is
   opaque; its layout (__crt_stdio_stream_data: _ptr, _base, _cnt, _flags)
   is checked against the count pointer the UCRT hands out, and a stream
   laid out otherwise counts as reading, as every stream did before. */
struct sp_w32_crt_stream { char *ptr; char *base; int cnt; long flags; };
#define SP_W32_IOREAD 0x0001
static int sp_w32_stream_reading(FILE *f, int *count) {
  struct sp_w32_crt_stream *st = (struct sp_w32_crt_stream *)f;
  if ((void *)&st->cnt != (void *)count) return 1;
  return (st->flags & SP_W32_IOREAD) != 0;
}
size_t __freadahead(FILE *f) {
  char **base = NULL, **ptr = NULL; int *count = NULL;
  if (_get_stream_buffer_pointers(f, &base, &ptr, &count) != 0 || !count) return 0;
  if (!sp_w32_stream_reading(f, count)) return 0;   /* a write buffer's free space */
  return *count > 0 ? (size_t)*count : 0;
}
size_t __fpending(FILE *f) {
  char **base = NULL, **ptr = NULL; int *count = NULL;
  if (_get_stream_buffer_pointers(f, &base, &ptr, &count) != 0 || !base || !ptr || !*base || !*ptr) return 0;
  return *ptr > *base ? (size_t)(*ptr - *base) : 0;
}
void __fpurge(FILE *f) {
  char **base = NULL, **ptr = NULL; int *count = NULL;
  if (_get_stream_buffer_pointers(f, &base, &ptr, &count) == 0 && ptr && base && count) { *ptr = *base; *count = 0; }
}

/* ---- temporary files and the environment ---- */

static void sp_w32_fill_x(char *t, size_t k) {
  static const char al[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  unsigned char r[16];
  if (BCryptGenRandom(NULL, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    unsigned long long x = GetTickCount64() ^ ((unsigned long long)GetCurrentProcessId() << 32) ^ (uintptr_t)t;
    for (size_t i = 0; i < sizeof r; i++) { x = x * 6364136223846793005ULL + 1442695040888963407ULL; r[i] = (unsigned char)(x >> 56); }
  }
  for (size_t i = 0; i < k; i++) t[i] = al[r[i % sizeof r] % (sizeof al - 1)];
}

char *mkdtemp(char *tmpl) {
  size_t n = tmpl ? strlen(tmpl) : 0;
  if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX") != 0) { errno = EINVAL; return NULL; }
  for (int tries = 0; tries < 100; tries++) {
    sp_w32_fill_x(tmpl + n - 6, 6);
    if (sp_w32_mkdir(tmpl, 0700) == 0) return tmpl;
    if (errno != EEXIST) return NULL;
  }
  errno = EEXIST;
  return NULL;
}

int sp_w32_mkstemp(char *tmpl) {
  size_t n = tmpl ? strlen(tmpl) : 0;
  if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX") != 0) { errno = EINVAL; return -1; }
  for (int tries = 0; tries < 100; tries++) {
    sp_w32_fill_x(tmpl + n - 6, 6);
    int fd = sp_w32_open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) return fd;
    if (errno != EEXIST) return -1;
  }
  errno = EEXIST;
  return -1;
}

int setenv(const char *name, const char *value, int overwrite) {
  if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
  if (!overwrite && getenv(name)) return 0;
  return _putenv_s(name, value ? value : "") == 0 ? 0 : (errno = EINVAL, -1);
}
int unsetenv(const char *name) {
  if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
  return _putenv_s(name, "") == 0 ? 0 : (errno = EINVAL, -1);
}

char ***sp_w32_environ(void) { return __p__environ(); }

int getentropy(void *buf, size_t n) {
  if (n > 256) { errno = EIO; return -1; }
  return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : (errno = EIO, -1);
}

/* ===================================================================== */
/* directories                                                           */
/* ===================================================================== */

struct sp_w32_dir {
  HANDLE h;
  int dfd;           /* dirfd's descriptor, or the one fdopendir took; -1 */
  wchar_t *pattern;
  WIN32_FIND_DATAW fd;
  int first, done;
  long pos;
  struct dirent ent;
};

static DIR *sp_w32_dir_open(const wchar_t *dirw) {
  size_t n = wcslen(dirw);
  wchar_t *pat = (wchar_t *)malloc(sizeof(wchar_t) * (n + 3));
  if (!pat) { errno = ENOMEM; return NULL; }
  wcscpy(pat, dirw);
  if (n && pat[n - 1] != L'\\' && pat[n - 1] != L'/' && pat[n - 1] != L':') wcscat(pat, L"\\");
  wcscat(pat, L"*");
  DIR *d = (DIR *)calloc(1, sizeof *d);
  if (!d) { free(pat); errno = ENOMEM; return NULL; }
  d->pattern = pat;
  d->dfd = -1;
  d->h = FindFirstFileExW(pat, FindExInfoBasic, &d->fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
  if (d->h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    free(pat); free(d);
    errno = e == ERROR_DIRECTORY ? ENOTDIR : sp_w32_errno_of(e);
    return NULL;
  }
  d->first = 1;
  return d;
}

DIR *sp_w32_opendir(const char *path) {
  wchar_t wb[SP_W32_WBUF], *w = sp_w32_wpath(path, wb, SP_W32_WBUF);
  if (!w) return NULL;
  DWORD a = GetFileAttributesW(w);
  if (a == INVALID_FILE_ATTRIBUTES) { errno = sp_w32_path_errno(w, sp_w32_errno_of(GetLastError()), 1); sp_w32_wfree(w, wb); return NULL; }
  if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { sp_w32_wfree(w, wb); errno = ENOTDIR; return NULL; }
  DIR *d = sp_w32_dir_open(w);
  sp_w32_wfree(w, wb);
  return d;
}

struct dirent *sp_w32_readdir(DIR *d) {
  if (!d) { errno = EBADF; return NULL; }
  if (d->done) return NULL;
  if (!d->first) {
    if (!FindNextFileW(d->h, &d->fd)) { d->done = 1; return NULL; }
  }
  d->first = 0;
  d->pos++;
  WideCharToMultiByte(CP_UTF8, 0, d->fd.cFileName, -1, d->ent.d_name, (int)sizeof d->ent.d_name, NULL, NULL);
  DWORD a = d->fd.dwFileAttributes;
  if ((a & FILE_ATTRIBUTE_REPARSE_POINT) && (d->fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK ||
                                             d->fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT))
    d->ent.d_type = DT_LNK;
  else if (a & FILE_ATTRIBUTE_DIRECTORY) d->ent.d_type = DT_DIR;
  else d->ent.d_type = DT_REG;
  d->ent.d_ino = 0;
  d->ent.d_reclen = sizeof d->ent;
  d->ent.d_namlen = (unsigned short)strlen(d->ent.d_name);
  return &d->ent;
}

int sp_w32_closedir(DIR *d) {
  if (!d) { errno = EBADF; return -1; }
  if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
  if (d->dfd >= 0) _close(d->dfd);
  free(d->pattern);
  free(d);
  return 0;
}

void sp_w32_rewinddir(DIR *d) {
  if (!d) return;
  if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
  d->h = FindFirstFileExW(d->pattern, FindExInfoBasic, &d->fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
  d->first = 1; d->done = d->h == INVALID_HANDLE_VALUE; d->pos = 0;
}

long sp_w32_telldir(DIR *d) { return d ? d->pos : -1; }
void sp_w32_seekdir(DIR *d, long pos) {
  if (!d) return;
  sp_w32_rewinddir(d);
  while (d->pos < pos && sp_w32_readdir(d)) {}
}
/* a directory listing has no descriptor; dirfd opens the directory itself
   for one, which closedir closes */
int sp_w32_dirfd(DIR *d) {
  if (!d) { errno = EINVAL; return -1; }
  if (d->dfd >= 0) return d->dfd;
  size_t n = wcslen(d->pattern);
  wchar_t *dir = _wcsdup(d->pattern);
  if (!dir) { errno = ENOMEM; return -1; }
  if (n >= 2) dir[n - 2] = 0;   /* drop "\\*" */
  HANDLE h = CreateFileW(dir, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(dir);
  if (h == INVALID_HANDLE_VALUE) return sp_w32_fail();
  int fd = _open_osfhandle((intptr_t)h, _O_RDONLY);
  if (fd < 0) { CloseHandle(h); errno = EMFILE; return -1; }
  d->dfd = fd;
  return fd;
}
/* a listing of the directory an fd names; the DIR owns the fd from here */
DIR *sp_w32_fdopendir(int fd) {
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return NULL; }
  BY_HANDLE_FILE_INFORMATION bi;
  if (!GetFileInformationByHandle(h, &bi)) { errno = EBADF; return NULL; }
  if (!(bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { errno = ENOTDIR; return NULL; }
  wchar_t p[MAX_PATH * 4];
  DWORD k = GetFinalPathNameByHandleW(h, p, (DWORD)(sizeof p / sizeof p[0]), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (k == 0 || k >= sizeof p / sizeof p[0]) { errno = EBADF; return NULL; }
  DIR *d = sp_w32_dir_open(p);
  if (d) d->dfd = fd;
  return d;
}

/* ===================================================================== */
/* users                                                                 */
/* ===================================================================== */

static struct passwd sp_w32_pw;
static char sp_w32_pw_name[256], sp_w32_pw_dir[MAX_PATH * 3], sp_w32_pw_shell[MAX_PATH * 3];

static struct passwd *sp_w32_me(void) {
  wchar_t w[256]; DWORD n = 256;
  if (GetUserNameW(w, &n)) WideCharToMultiByte(CP_UTF8, 0, w, -1, sp_w32_pw_name, sizeof sp_w32_pw_name, NULL, NULL);
  else snprintf(sp_w32_pw_name, sizeof sp_w32_pw_name, "%s", getenv("USERNAME") ? getenv("USERNAME") : "user");
  const wchar_t *home = _wgetenv(L"USERPROFILE");
  if (home) { WideCharToMultiByte(CP_UTF8, 0, home, -1, sp_w32_pw_dir, sizeof sp_w32_pw_dir, NULL, NULL); sp_w32_slashes(sp_w32_pw_dir); }
  else sp_w32_pw_dir[0] = 0;
  const wchar_t *sh = _wgetenv(L"ComSpec");
  if (sh) { WideCharToMultiByte(CP_UTF8, 0, sh, -1, sp_w32_pw_shell, sizeof sp_w32_pw_shell, NULL, NULL); sp_w32_slashes(sp_w32_pw_shell); }
  else snprintf(sp_w32_pw_shell, sizeof sp_w32_pw_shell, "C:/Windows/System32/cmd.exe");
  sp_w32_pw.pw_name = sp_w32_pw_name;
  sp_w32_pw.pw_passwd = (char *)"";
  sp_w32_pw.pw_uid = 0; sp_w32_pw.pw_gid = 0;
  sp_w32_pw.pw_gecos = sp_w32_pw_name;
  sp_w32_pw.pw_dir = sp_w32_pw_dir;
  sp_w32_pw.pw_shell = sp_w32_pw_shell;
  return &sp_w32_pw;
}
struct passwd *getpwuid(uid_t uid) { if (uid != 0) { errno = 0; return NULL; } return sp_w32_me(); }
struct passwd *getpwnam(const char *name) {
  struct passwd *p = sp_w32_me();
  if (!name || _stricmp(name, p->pw_name) != 0) { errno = 0; return NULL; }
  return p;
}
struct group *getgrgid(gid_t gid) { (void)gid; errno = 0; return NULL; }
struct group *getgrnam(const char *name) { (void)name; errno = 0; return NULL; }
/* the walks of the two databases: the one user getpwuid knows, and no group */
static int sp_w32_pw_walked;
void setpwent(void) { sp_w32_pw_walked = 0; }
void endpwent(void) { sp_w32_pw_walked = 0; }
struct passwd *getpwent(void) {
  if (sp_w32_pw_walked) return NULL;
  sp_w32_pw_walked = 1;
  return sp_w32_me();
}
void setgrent(void) {}
void endgrent(void) {}
struct group *getgrent(void) { return NULL; }

uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }
int getgroups(int n, gid_t *list) { if (n > 0 && list) list[0] = 0; return n > 0 ? 1 : 1; }
int chown(const char *path, uid_t uid, gid_t gid) {
  (void)uid; (void)gid;
  struct sp_w32_stat st;
  return sp_w32_stat(path, &st);   /* there is no owner to change; the path must exist */
}
int lchown(const char *path, uid_t uid, gid_t gid) {
  (void)uid; (void)gid;
  struct sp_w32_stat st;
  return sp_w32_lstat(path, &st);
}
int fchown(int fd, uid_t uid, gid_t gid) {
  (void)uid; (void)gid;
  if ((HANDLE)_get_osfhandle(fd) == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
  return 0;
}
char *getlogin(void) { return sp_w32_me()->pw_name; }
char *ttyname(int fd) { return sp_w32_isatty(fd) ? (char *)"con" : (errno = ENOTTY, (char *)NULL); }

/* ===================================================================== */
/* system information                                                    */
/* ===================================================================== */

long sysconf(int name) {
  SYSTEM_INFO si;
  switch (name) {
    case _SC_PAGESIZE: GetSystemInfo(&si); return (long)si.dwPageSize;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: {
      DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
      return n ? (long)n : 1;
    }
    case _SC_CLK_TCK: return 100;
    case _SC_OPEN_MAX: return 8192;
    case _SC_PHYS_PAGES: {
      MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
      GetSystemInfo(&si);
      if (!GlobalMemoryStatusEx(&ms)) return -1;
      return (long)(ms.ullTotalPhys / si.dwPageSize);
    }
    case _SC_ARG_MAX: return 32767;
    default: errno = EINVAL; return -1;
  }
}
int getpagesize(void) { return (int)sysconf(_SC_PAGESIZE); }

/* ---- times and usage ---- */

static long long sp_w32_ft100ns(FILETIME ft) {
  ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
  return (long long)u.QuadPart;
}

/* CPU time of the children waited for, which times() and getrusage report
   as the children's */
static long long sp_w32_child_user, sp_w32_child_sys;

clock_t times(struct tms *t) {
  FILETIME c, e, k, u;
  if (t) {
    memset(t, 0, sizeof *t);
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
      t->tms_utime = (clock_t)(sp_w32_ft100ns(u) / 100000);
      t->tms_stime = (clock_t)(sp_w32_ft100ns(k) / 100000);
    }
    t->tms_cutime = (clock_t)(sp_w32_child_user / 100000);
    t->tms_cstime = (clock_t)(sp_w32_child_sys / 100000);
  }
  return (clock_t)(GetTickCount64() / 10);
}

int getrusage(int who, struct rusage *ru) {
  if (!ru) { errno = EFAULT; return -1; }
  memset(ru, 0, sizeof *ru);
  long long ut = 0, st = 0;
  FILETIME c, e, k, u;
  if (who == RUSAGE_CHILDREN) { ut = sp_w32_child_user; st = sp_w32_child_sys; }
  else if (who == RUSAGE_THREAD) {
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return sp_w32_fail();
    ut = sp_w32_ft100ns(u); st = sp_w32_ft100ns(k);
  } else {
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return sp_w32_fail();
    ut = sp_w32_ft100ns(u); st = sp_w32_ft100ns(k);
    PROCESS_MEMORY_COUNTERS pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
      ru->ru_maxrss = (long)(pmc.PeakWorkingSetSize / 1024);
      ru->ru_majflt = (long)pmc.PageFaultCount;
    }
  }
  ru->ru_utime.tv_sec = (long)(ut / 10000000); ru->ru_utime.tv_usec = (long)((ut % 10000000) / 10);
  ru->ru_stime.tv_sec = (long)(st / 10000000); ru->ru_stime.tv_usec = (long)((st % 10000000) / 10);
  return 0;
}

int getrlimit(int res, struct rlimit *rl) {
  if (!rl) { errno = EFAULT; return -1; }
  rl->rlim_cur = rl->rlim_max = RLIM_INFINITY;
  if (res == RLIMIT_STACK) {
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    rl->rlim_cur = rl->rlim_max = (rlim_t)(hi - lo);
  } else if (res == RLIMIT_NOFILE) {
    rl->rlim_cur = rl->rlim_max = 8192;
  } else if (res < 0 || res >= RLIM_NLIMITS) { errno = EINVAL; return -1; }
  return 0;
}
int setrlimit(int res, const struct rlimit *rl) {
  if (!rl) { errno = EFAULT; return -1; }
  if (res < 0 || res >= RLIM_NLIMITS) { errno = EINVAL; return -1; }
  if (rl->rlim_cur > rl->rlim_max) { errno = EINVAL; return -1; }
  return 0;   /* accepted; Windows has no per-process knob for it */
}

/* nice values and priority classes, each class answering for the band of
   nice values around its canonical one */
static int sp_w32_nice_of(DWORD cls) {
  switch (cls) {
    case REALTIME_PRIORITY_CLASS: return -20;
    case HIGH_PRIORITY_CLASS: return -10;
    case ABOVE_NORMAL_PRIORITY_CLASS: return -5;
    case BELOW_NORMAL_PRIORITY_CLASS: return 5;
    case IDLE_PRIORITY_CLASS: return 19;
    default: return 0;
  }
}
static DWORD sp_w32_class_of(int nice) {
  if (nice <= -15) return HIGH_PRIORITY_CLASS;
  if (nice < 0) return ABOVE_NORMAL_PRIORITY_CLASS;
  if (nice == 0) return NORMAL_PRIORITY_CLASS;
  if (nice < 15) return BELOW_NORMAL_PRIORITY_CLASS;
  return IDLE_PRIORITY_CLASS;
}
static int sp_w32_own_nice = 0, sp_w32_own_nice_set = 0;
int getpriority(int which, id_t who) {
  if (which != PRIO_PROCESS) { errno = EINVAL; return -1; }
  HANDLE h = who == 0 ? GetCurrentProcess() : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)who);
  if (!h) { errno = ESRCH; return -1; }
  DWORD cls = GetPriorityClass(h);
  if (who != 0) CloseHandle(h);
  if (!cls) return sp_w32_fail();
  int n = sp_w32_nice_of(cls);
  if ((who == 0 || who == (id_t)GetCurrentProcessId()) && sp_w32_own_nice_set &&
      sp_w32_class_of(sp_w32_own_nice) == cls) n = sp_w32_own_nice;
  errno = 0;
  return n;
}
int setpriority(int which, id_t who, int prio) {
  if (which != PRIO_PROCESS) { errno = EINVAL; return -1; }
  if (prio < -20) prio = -20;
  if (prio > 19) prio = 19;
  HANDLE h = who == 0 ? GetCurrentProcess() : OpenProcess(PROCESS_SET_INFORMATION, FALSE, (DWORD)who);
  if (!h) { errno = ESRCH; return -1; }
  BOOL ok = SetPriorityClass(h, sp_w32_class_of(prio));
  if (who != 0) CloseHandle(h);
  if (!ok) { errno = EACCES; return -1; }
  if (who == 0 || who == (id_t)GetCurrentProcessId()) { sp_w32_own_nice = prio; sp_w32_own_nice_set = 1; }
  return 0;
}

/* ===================================================================== */
/* processes                                                             */
/* ===================================================================== */

/* The children this process started, by pid: their handles, and the
   signal kill(2) ended one with, for waitpid's status. */
typedef struct { DWORD pid; HANDLE h; int sig; } sp_w32_child;
static sp_w32_child *sp_w32_children;
static int sp_w32_nchildren, sp_w32_children_cap;
static SRWLOCK sp_w32_children_lock = SRWLOCK_INIT;

static void sp_w32_child_add(DWORD pid, HANDLE h) {
  AcquireSRWLockExclusive(&sp_w32_children_lock);
  if (sp_w32_nchildren == sp_w32_children_cap) {
    int nc = sp_w32_children_cap ? sp_w32_children_cap * 2 : 16;
    sp_w32_child *nx = (sp_w32_child *)realloc(sp_w32_children, sizeof *nx * (size_t)nc);
    if (!nx) { ReleaseSRWLockExclusive(&sp_w32_children_lock); return; }
    sp_w32_children = nx; sp_w32_children_cap = nc;
  }
  sp_w32_children[sp_w32_nchildren].pid = pid;
  sp_w32_children[sp_w32_nchildren].h = h;
  sp_w32_children[sp_w32_nchildren].sig = 0;
  sp_w32_nchildren++;
  ReleaseSRWLockExclusive(&sp_w32_children_lock);
}

static int sp_w32_child_find(DWORD pid) {
  for (int i = 0; i < sp_w32_nchildren; i++) if (sp_w32_children[i].pid == pid) return i;
  return -1;
}

/* the status waitpid answers for a child that has ended, and its CPU time
   counted as the children's */
static int sp_w32_child_reap(int i) {
  sp_w32_child c = sp_w32_children[i];
  DWORD code = 0;
  GetExitCodeProcess(c.h, &code);
  FILETIME cr, ex, k, u;
  if (GetProcessTimes(c.h, &cr, &ex, &k, &u)) { sp_w32_child_user += sp_w32_ft100ns(u); sp_w32_child_sys += sp_w32_ft100ns(k); }
  CloseHandle(c.h);
  sp_w32_children[i] = sp_w32_children[--sp_w32_nchildren];
  if (c.sig) return c.sig & 0x7f;
  /* an NTSTATUS crash code is what POSIX would call a signal */
  if (code == 0xC0000005 || code == 0xC00000FD) return SIGSEGV;
  if (code == 0xC000013A) return SIGINT;
  if (code == 0xC0000094 || code == 0xC0000095 || (code >= 0xC000008D && code <= 0xC0000093)) return SIGFPE;
  if (code == 0xC000001D || code == 0xC0000096) return SIGILL;
  return (int)((code & 0xff) << 8);
}

pid_t waitpid(pid_t pid, int *status, int options) {
  for (;;) {
    AcquireSRWLockExclusive(&sp_w32_children_lock);
    if (pid > 0) {
      int i = sp_w32_child_find((DWORD)pid);
      if (i < 0) { ReleaseSRWLockExclusive(&sp_w32_children_lock); errno = ECHILD; return -1; }
      HANDLE h = sp_w32_children[i].h;
      ReleaseSRWLockExclusive(&sp_w32_children_lock);
      DWORD w = WaitForSingleObject(h, (options & WNOHANG) ? 0 : INFINITE);
      if (w == WAIT_TIMEOUT) return 0;
      if (w != WAIT_OBJECT_0) return sp_w32_fail();
      AcquireSRWLockExclusive(&sp_w32_children_lock);
      i = sp_w32_child_find((DWORD)pid);
      if (i < 0) { ReleaseSRWLockExclusive(&sp_w32_children_lock); errno = ECHILD; return -1; }
      int st = sp_w32_child_reap(i);
      ReleaseSRWLockExclusive(&sp_w32_children_lock);
      if (status) *status = st;
      return pid;
    }
    /* any child (pid -1, or this group: there are no other groups here) */
    if (sp_w32_nchildren == 0) { ReleaseSRWLockExclusive(&sp_w32_children_lock); errno = ECHILD; return -1; }
    int n = sp_w32_nchildren > MAXIMUM_WAIT_OBJECTS ? MAXIMUM_WAIT_OBJECTS : sp_w32_nchildren;
    HANDLE hs[MAXIMUM_WAIT_OBJECTS];
    DWORD pids[MAXIMUM_WAIT_OBJECTS];
    for (int i = 0; i < n; i++) { hs[i] = sp_w32_children[i].h; pids[i] = sp_w32_children[i].pid; }
    int more = sp_w32_nchildren > n;
    ReleaseSRWLockExclusive(&sp_w32_children_lock);
    DWORD tmo = (options & WNOHANG) ? 0 : (more ? 50 : INFINITE);
    DWORD w = WaitForMultipleObjects((DWORD)n, hs, FALSE, tmo);
    if (w == WAIT_TIMEOUT) { if (options & WNOHANG) return 0; continue; }
    if (w >= WAIT_OBJECT_0 + (DWORD)n) return sp_w32_fail();
    DWORD got = pids[w - WAIT_OBJECT_0];
    AcquireSRWLockExclusive(&sp_w32_children_lock);
    int i = sp_w32_child_find(got);
    if (i < 0) { ReleaseSRWLockExclusive(&sp_w32_children_lock); continue; }
    int st = sp_w32_child_reap(i);
    ReleaseSRWLockExclusive(&sp_w32_children_lock);
    if (status) *status = st;
    return (pid_t)got;
  }
}
pid_t wait(int *status) { return waitpid(-1, status, 0); }

pid_t getppid(void) {
  static DWORD ppid = 0;
  if (ppid) return (pid_t)ppid;
  DWORD me = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W pe; pe.dwSize = sizeof pe;
  if (Process32FirstW(snap, &pe)) do {
    if (pe.th32ProcessID == me) { ppid = pe.th32ParentProcessID; break; }
  } while (Process32NextW(snap, &pe));
  CloseHandle(snap);
  return (pid_t)ppid;
}
pid_t getpgrp(void) { return (pid_t)GetCurrentProcessId(); }
pid_t getpgid(pid_t pid) { return pid == 0 ? (pid_t)GetCurrentProcessId() : pid; }
int setpgid(pid_t pid, pid_t pgid) { (void)pid; (void)pgid; return 0; }
pid_t setsid(void) { return (pid_t)GetCurrentProcessId(); }
pid_t getsid(pid_t pid) { return pid == 0 ? (pid_t)GetCurrentProcessId() : pid; }

pid_t sp_w32_fork(void) { errno = ENOSYS; return -1; }
int fchdir(int fd) { (void)fd; errno = ENOSYS; return -1; }

/* ---- command lines ---- */

/* argv quoted for the child the way the MSVC runtime (CommandLineToArgvW)
   splits a command line back into words */
static void sp_w32_quote_arg(wchar_t **out, size_t *len, size_t *cap, const wchar_t *a) {
#define SP_W32_PUT(c) do { if (*len + 2 > *cap) { *cap = *cap * 2 + 64; *out = (wchar_t *)realloc(*out, sizeof(wchar_t) * *cap); } (*out)[(*len)++] = (c); } while (0)
  if (*len) SP_W32_PUT(L' ');
  if (*a && !wcspbrk(a, L" \t\n\v\"")) { for (; *a; a++) SP_W32_PUT(*a); return; }
  SP_W32_PUT(L'"');
  for (;;) {
    size_t bs = 0;
    while (*a == L'\\') { a++; bs++; }
    if (!*a) { for (size_t i = 0; i < bs * 2; i++) SP_W32_PUT(L'\\'); break; }
    if (*a == L'"') { for (size_t i = 0; i < bs * 2 + 1; i++) SP_W32_PUT(L'\\'); SP_W32_PUT(L'"'); }
    else { for (size_t i = 0; i < bs; i++) SP_W32_PUT(L'\\'); SP_W32_PUT(*a); }
    a++;
  }
  SP_W32_PUT(L'"');
#undef SP_W32_PUT
}

/* "/dev/null" in a shell command line, spelled the way cmd.exe knows it */
static char *sp_w32_shell_text(const char *cmd) {
  size_t n = strlen(cmd), k = 0;
  char *o = (char *)malloc(n + 1);
  if (!o) return NULL;
  for (size_t i = 0; i < n;) {
    if (strncmp(cmd + i, "/dev/null", 9) == 0) { memcpy(o + k, "NUL", 3); k += 3; i += 9; }
    else o[k++] = cmd[i++];
  }
  o[k] = 0;
  return o;
}

static wchar_t *sp_w32_comspec(void) {
  static wchar_t cs[MAX_PATH];
  if (!cs[0]) {
    DWORD n = GetEnvironmentVariableW(L"ComSpec", cs, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
      UINT k = GetSystemDirectoryW(cs, MAX_PATH - 10);
      wcscpy(cs + k, L"\\cmd.exe");
    }
  }
  return cs;
}

/* a program name resolved the way a POSIX execvp would: PATH searched,
   with PATHEXT's extensions tried for a bare name */
static int sp_w32_find_program(const wchar_t *name, int search, wchar_t *out, size_t cap) {
  int has_dir = wcschr(name, L'\\') || wcschr(name, L'/') || (name[0] && name[1] == L':');
  const wchar_t *dot = wcsrchr(name, L'.');
  const wchar_t *sl = wcsrchr(name, L'\\'), *sl2 = wcsrchr(name, L'/');
  if (sl2 > sl) sl = sl2;
  int has_ext = dot && (!sl || dot > sl);
  static const wchar_t *exts[] = { L"", L".exe", L".com", L".bat", L".cmd" };
  wchar_t cand[MAX_PATH * 2];
  if (has_dir || !search) {
    for (int e = 0; e < 5; e++) {
      if (e > 0 && has_ext) break;
      _snwprintf(cand, MAX_PATH * 2, L"%ls%ls", name, exts[e]);
      DWORD a = GetFileAttributesW(cand);
      if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) { wcsncpy(out, cand, cap); out[cap - 1] = 0; return 1; }
    }
    /* /bin/echo, /usr/bin/env: a POSIX program spelled by its POSIX home,
       which a Windows drive does not have -- the one on PATH by that name */
    const wchar_t *bn = NULL;
    if (!wcsncmp(name, L"/bin/", 5)) bn = name + 5;
    else if (!wcsncmp(name, L"/usr/bin/", 9)) bn = name + 9;
    else if (!wcsncmp(name, L"/usr/local/bin/", 15)) bn = name + 15;
    if (bn && *bn && !wcschr(bn, L'/') && search >= 0) return sp_w32_find_program(bn, 1, out, cap);
    return 0;
  }
  for (int e = 0; e < 5; e++) {
    if (e > 0 && has_ext) break;
    if (e == 0 && !has_ext) continue;
    DWORD k = SearchPathW(NULL, name, e ? exts[e] : NULL, (DWORD)cap, out, NULL);
    if (k > 0 && k < cap) {
      DWORD a = GetFileAttributesW(out);
      if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return 1;
    }
  }
  return 0;
}

/* ---- posix_spawn ---- */

struct sp_w32_spawn_action {
  int kind;   /* 1 dup2, 2 close, 3 open, 4 chdir */
  int fd, newfd, oflag;
  mode_t mode;
  char *path;
};

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa) { memset(fa, 0, sizeof *fa); return 0; }
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa) {
  for (int i = 0; i < fa->n; i++) free(fa->acts[i].path);
  free(fa->acts);
  memset(fa, 0, sizeof *fa);
  return 0;
}
static sp_w32_spawn_action *sp_w32_fa_push(posix_spawn_file_actions_t *fa) {
  if (fa->n == fa->cap) {
    int nc = fa->cap ? fa->cap * 2 : 8;
    sp_w32_spawn_action *nx = (sp_w32_spawn_action *)realloc(fa->acts, sizeof *nx * (size_t)nc);
    if (!nx) return NULL;
    fa->acts = nx; fa->cap = nc;
  }
  sp_w32_spawn_action *a = &fa->acts[fa->n++];
  memset(a, 0, sizeof *a);
  return a;
}
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd) {
  sp_w32_spawn_action *a = sp_w32_fa_push(fa); if (!a) return ENOMEM;
  a->kind = 1; a->fd = fd; a->newfd = newfd; return 0;
}
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd) {
  sp_w32_spawn_action *a = sp_w32_fa_push(fa); if (!a) return ENOMEM;
  a->kind = 2; a->fd = fd; return 0;
}
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd, const char *path, int oflag, mode_t mode) {
  sp_w32_spawn_action *a = sp_w32_fa_push(fa); if (!a) return ENOMEM;
  a->kind = 3; a->fd = fd; a->oflag = oflag; a->mode = mode; a->path = strdup(path ? path : "");
  return a->path ? 0 : ENOMEM;
}
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *fa, const char *path) {
  sp_w32_spawn_action *a = sp_w32_fa_push(fa); if (!a) return ENOMEM;
  a->kind = 4; a->path = strdup(path ? path : "");
  return a->path ? 0 : ENOMEM;
}
int posix_spawnattr_init(posix_spawnattr_t *a) { memset(a, 0, sizeof *a); return 0; }
int posix_spawnattr_destroy(posix_spawnattr_t *a) { (void)a; return 0; }
int posix_spawnattr_setflags(posix_spawnattr_t *a, short flags) { a->flags = flags; return 0; }
int posix_spawnattr_getflags(const posix_spawnattr_t *a, short *flags) { *flags = a->flags; return 0; }
int posix_spawnattr_setpgroup(posix_spawnattr_t *a, pid_t pg) { a->pgroup = pg; return 0; }
int posix_spawnattr_setsigmask(posix_spawnattr_t *a, const sigset_t *m) { (void)a; (void)m; return 0; }
int posix_spawnattr_setsigdefault(posix_spawnattr_t *a, const sigset_t *m) { (void)a; (void)m; return 0; }

static wchar_t *sp_w32_w(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  if (n <= 0) return NULL;
  wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n);
  if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}

static int sp_w32_spawn(pid_t *pidp, const char *file, int search, const posix_spawn_file_actions_t *fa,
                        const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
  if (!file || !argv) return EINVAL;
  /* this process's own environment is inherited as it is: the CRT's narrow
     copy is in the ANSI code page, and rebuilding the block from it would
     lose what that cannot spell */
  if (envp == (char *const *)environ) envp = NULL;
  /* the child's standard handles: the parent's, then the file actions */
  HANDLE std[3] = {
    (HANDLE)_get_osfhandle(0), (HANDLE)_get_osfhandle(1), (HANDLE)_get_osfhandle(2)
  };
  HANDLE opened[16]; int nopened = 0;
  char *cwd = NULL;
  int rc = 0;
  if (fa) for (int i = 0; i < fa->n && rc == 0; i++) {
    sp_w32_spawn_action *a = &fa->acts[i];
    switch (a->kind) {
      case 1:
        if (a->newfd >= 0 && a->newfd < 3) {
          HANDLE src = a->fd >= 0 && a->fd < 3 ? std[a->fd] : (HANDLE)_get_osfhandle(a->fd);
          if (src == INVALID_HANDLE_VALUE) rc = EBADF;
          else std[a->newfd] = src;
        }
        break;
      case 2:
        if (a->fd >= 0 && a->fd < 3) std[a->fd] = INVALID_HANDLE_VALUE;
        break;
      case 3: {
        int fd = sp_w32_open(a->path, a->oflag, (int)a->mode);
        if (fd < 0) { rc = errno; break; }
        HANDLE h = (HANDLE)_get_osfhandle(fd), dh;
        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dh, 0, FALSE, DUPLICATE_SAME_ACCESS);
        _close(fd);
        if (nopened < 16) opened[nopened++] = dh;
        if (a->fd >= 0 && a->fd < 3) std[a->fd] = dh;
        break;
      }
      case 4:
        free(cwd);
        cwd = strdup(a->path);
        break;
    }
  }
  wchar_t *app = NULL, *cmdline = NULL, *wcwd = NULL, *wenv = NULL;
  size_t len = 0, cap = 0;
  /* /bin/sh -c CMD: CMD for the system's shell */
  int via_shell = (strcmp(file, "/bin/sh") == 0 || strcmp(file, "sh") == 0) &&
                  argv[0] && argv[1] && strcmp(argv[1], "-c") == 0 && argv[2];
  /* SPINEL_SHELL names a POSIX sh to run shell command lines under in
     place of the system's shell (a test harness under MSYS2 sets it) */
  wchar_t posix_sh[MAX_PATH * 2];
  int use_posix_sh = 0;
  if (via_shell) {
    DWORD n = GetEnvironmentVariableW(L"SPINEL_SHELL", posix_sh, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2 && GetFileAttributesW(posix_sh) != INVALID_FILE_ATTRIBUTES) use_posix_sh = 1;
  }
  if (rc == 0 && use_posix_sh) {
    wchar_t *wt = sp_w32_w(argv[2]);
    if (!wt) rc = EILSEQ;
    else {
      cap = wcslen(wt) * 2 + wcslen(posix_sh) * 2 + 32;
      cmdline = (wchar_t *)malloc(sizeof(wchar_t) * cap);
      len = 0;
      sp_w32_quote_arg(&cmdline, &len, &cap, posix_sh);
      sp_w32_quote_arg(&cmdline, &len, &cap, L"-c");
      sp_w32_quote_arg(&cmdline, &len, &cap, wt);
      cmdline[len] = 0;
      app = _wcsdup(posix_sh);
      free(wt);
    }
  } else if (rc == 0) {
    if (via_shell) {
      char *t = sp_w32_shell_text(argv[2]);
      wchar_t *wt = t ? sp_w32_w(t) : NULL;
      free(t);
      if (!wt) rc = ENOMEM;
      else {
        const wchar_t *cs = sp_w32_comspec();
        size_t n = wcslen(cs) + wcslen(wt) + 32;
        cmdline = (wchar_t *)malloc(sizeof(wchar_t) * n);
        if (cmdline) _snwprintf(cmdline, n, L"\"%ls\" /d /s /c \"%ls\"", cs, wt);
        app = _wcsdup(cs);
        free(wt);
        if (!cmdline || !app) rc = ENOMEM;
      }
    } else {
      wchar_t *wf = sp_w32_w(file);
      wchar_t found[MAX_PATH * 2];
      if (!wf) rc = EILSEQ;
      else if (!sp_w32_find_program(wf, search, found, MAX_PATH * 2)) rc = ENOENT;
      else {
        /* a batch file runs under the shell, as CreateProcess would itself */
        const wchar_t *dot = wcsrchr(found, L'.');
        int batch = dot && (!_wcsicmp(dot, L".bat") || !_wcsicmp(dot, L".cmd"));
        app = _wcsdup(batch ? sp_w32_comspec() : found);
        /* a batch file's words go to the shell whole: cmd /d /s /c "WORDS" */
        if (batch) sp_w32_quote_arg(&cmdline, &len, &cap, found);
        for (int i = batch ? 1 : 0; argv[i]; i++) {
          wchar_t *wa = sp_w32_w(argv[i]);
          if (!wa) { rc = EILSEQ; break; }
          sp_w32_quote_arg(&cmdline, &len, &cap, wa);
          free(wa);
        }
        if (rc == 0 && batch) {
          const wchar_t *cs = sp_w32_comspec();
          size_t n = wcslen(cs) + len + 32;
          wchar_t *outer = (wchar_t *)malloc(sizeof(wchar_t) * n);
          if (!outer) rc = ENOMEM;
          else { _snwprintf(outer, n, L"\"%ls\" /d /s /c \"%.*ls\"", cs, (int)len, cmdline); outer[n - 1] = 0; }
          free(cmdline); cmdline = outer; len = outer ? wcslen(outer) : 0; cap = len + 1;
        }
        if (rc == 0) {
          if (!cmdline) { cap = 1; cmdline = (wchar_t *)malloc(sizeof(wchar_t)); }
          if (len + 1 > cap) cmdline = (wchar_t *)realloc(cmdline, sizeof(wchar_t) * (len + 1));
          cmdline[len] = 0;
        }
      }
      free(wf);
    }
  }
  if (rc == 0 && cwd) { wcwd = sp_w32_w(cwd); if (!wcwd) rc = EILSEQ; }
  if (rc == 0 && cwd) {
    DWORD a = GetFileAttributesW(wcwd);
    if (a == INVALID_FILE_ATTRIBUTES) rc = ENOENT;
    else if (!(a & FILE_ATTRIBUTE_DIRECTORY)) rc = ENOTDIR;
  }
  if (rc == 0 && envp) {
    size_t tot = 1;
    for (int i = 0; envp[i]; i++) tot += (size_t)MultiByteToWideChar(CP_UTF8, 0, envp[i], -1, NULL, 0);
    wenv = (wchar_t *)malloc(sizeof(wchar_t) * tot);
    if (!wenv) rc = ENOMEM;
    else {
      size_t k = 0;
      for (int i = 0; envp[i]; i++) k += (size_t)MultiByteToWideChar(CP_UTF8, 0, envp[i], -1, wenv + k, (int)(tot - k));
      wenv[k] = 0;
    }
  }
  if (rc == 0) {
    /* only the three standard handles reach the child, each inheritable for
       this one call: a pipe end the parent holds must not leak into an
       unrelated child, where it would keep the pipe open */
    HANDLE inh[3]; HANDLE list[3]; int nlist = 0;
    for (int i = 0; i < 3; i++) {
      inh[i] = NULL;
      if (std[i] && std[i] != INVALID_HANDLE_VALUE &&
          DuplicateHandle(GetCurrentProcess(), std[i], GetCurrentProcess(), &inh[i], 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        int dup = 0;
        for (int j = 0; j < nlist; j++) if (list[j] == inh[i]) dup = 1;
        if (!dup) list[nlist++] = inh[i];
      }
    }
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    LPPROC_THREAD_ATTRIBUTE_LIST al = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(asz);
    int have_al = al && InitializeProcThreadAttributeList(al, 1, 0, &asz) &&
                  (nlist == 0 || UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, sizeof(HANDLE) * (size_t)nlist, NULL, NULL));
    STARTUPINFOEXW si; memset(&si, 0, sizeof si);
    si.StartupInfo.cb = have_al ? sizeof si : sizeof si.StartupInfo;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inh[0];
    si.StartupInfo.hStdOutput = inh[1];
    si.StartupInfo.hStdError = inh[2];
    si.lpAttributeList = have_al ? al : NULL;
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | (have_al ? EXTENDED_STARTUPINFO_PRESENT : 0);
    if (attr && (attr->flags & (POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSID))) flags |= CREATE_NEW_PROCESS_GROUP;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(app, cmdline, NULL, NULL, nlist > 0 || !have_al, flags, wenv, wcwd,
                             &si.StartupInfo, &pi);
    DWORD err = GetLastError();
    if (have_al) DeleteProcThreadAttributeList(al);
    free(al);
    for (int i = 0; i < 3; i++) if (inh[i]) CloseHandle(inh[i]);
    if (!ok) rc = sp_w32_errno_of(err);
    else {
      CloseHandle(pi.hThread);
      sp_w32_child_add(pi.dwProcessId, pi.hProcess);
      if (pidp) *pidp = (pid_t)pi.dwProcessId;
    }
  }
  for (int i = 0; i < nopened; i++) CloseHandle(opened[i]);
  free(app); free(cmdline); free(wcwd); free(wenv); free(cwd);
  return rc;
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
  return sp_w32_spawn(pid, path, 0, fa, attr, argv, envp);
}
int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
  return sp_w32_spawn(pid, file, 1, fa, attr, argv, envp);
}

/* exec: Windows cannot replace a process image, so the program runs as a
   child with this process's standard handles and this process exits with
   its status when it ends -- what the MSVC runtime's _exec does. Output
   still in a stdio buffer is lost, as execve loses it: nothing is flushed,
   and the process ends by TerminateProcess, since even _exit lets the
   UCRT's DLL detach flush every stream. */
static void sp_w32_exec_exit(int code) {
  TerminateProcess(GetCurrentProcess(), (UINT)code);
  _exit(code);
}
static int sp_w32_exec(const char *path, int search, char *const argv[], char *const envp[]) {
  pid_t pid;
  int rc = sp_w32_spawn(&pid, path, search, NULL, NULL, argv, envp);
  if (rc != 0) { errno = rc; return -1; }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) sp_w32_exec_exit(127);
  sp_w32_exec_exit(WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
  return -1;
}
int sp_w32_execvp(const char *file, char *const argv[]) { return sp_w32_exec(file, 1, argv, NULL); }
int sp_w32_execv(const char *path, char *const argv[]) { return sp_w32_exec(path, 0, argv, NULL); }
int sp_w32_execve(const char *path, char *const argv[], char *const envp[]) { return sp_w32_exec(path, 0, argv, envp); }
int sp_w32_execl(const char *path, const char *arg, ...) {
  const char *av[64]; int n = 0;
  va_list ap; va_start(ap, arg);
  av[n++] = arg;
  while (n < 63 && (av[n] = va_arg(ap, const char *)) != NULL) n++;
  va_end(ap);
  av[n] = NULL;
  return sp_w32_exec(path, 0, (char *const *)av, NULL);
}

int sp_w32_system(const char *cmd) {
  if (!cmd) return 1;
  char *argv[] = { (char *)"sh", (char *)"-c", (char *)cmd, NULL };
  pid_t pid;
  fflush(NULL);
  int rc = sp_w32_spawn(&pid, "/bin/sh", 0, NULL, NULL, argv, NULL);
  if (rc != 0) { errno = rc; return -1; }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) return -1;
  return st;
}

/* popen: the command under the shell with its stdin or stdout on a pipe */
typedef struct { FILE *f; pid_t pid; } sp_w32_popen_ent;
static sp_w32_popen_ent sp_w32_popens[64];
static SRWLOCK sp_w32_popen_lock = SRWLOCK_INIT;

FILE *sp_w32_popen(const char *cmd, const char *mode) {
  if (!cmd || !mode || (mode[0] != 'r' && mode[0] != 'w')) { errno = EINVAL; return NULL; }
  int fds[2];
  if (_pipe(fds, 65536, _O_BINARY | _O_NOINHERIT) != 0) return NULL;
  int reading = mode[0] == 'r';
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, reading ? fds[1] : fds[0], reading ? 1 : 0);
  char *argv[] = { (char *)"sh", (char *)"-c", (char *)cmd, NULL };
  pid_t pid;
  fflush(NULL);
  int rc = sp_w32_spawn(&pid, "/bin/sh", 0, &fa, NULL, argv, NULL);
  posix_spawn_file_actions_destroy(&fa);
  _close(reading ? fds[1] : fds[0]);
  int keep = reading ? fds[0] : fds[1];
  if (rc != 0) { _close(keep); errno = rc; return NULL; }
  FILE *f = _fdopen(keep, reading ? "rb" : "wb");
  if (!f) { _close(keep); return NULL; }
  AcquireSRWLockExclusive(&sp_w32_popen_lock);
  for (int i = 0; i < 64; i++) if (!sp_w32_popens[i].f) { sp_w32_popens[i].f = f; sp_w32_popens[i].pid = pid; break; }
  ReleaseSRWLockExclusive(&sp_w32_popen_lock);
  return f;
}

int sp_w32_pclose(FILE *f) {
  pid_t pid = -1;
  AcquireSRWLockExclusive(&sp_w32_popen_lock);
  for (int i = 0; i < 64; i++) if (sp_w32_popens[i].f == f) { pid = sp_w32_popens[i].pid; sp_w32_popens[i].f = NULL; break; }
  ReleaseSRWLockExclusive(&sp_w32_popen_lock);
  if (pid < 0) { errno = ECHILD; return -1; }
  fclose(f);
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) return -1;
  return st;
}

/* ===================================================================== */
/* signals                                                               */
/* ===================================================================== */

/* The handlers sigaction installed, for every signal number the shim
   names. The CRT delivers the ones it knows (SIGINT, SIGTERM, SIGBREAK,
   SIGABRT, SIGFPE, SIGILL, SIGSEGV) to a trampoline that calls the stored
   handler; kill(2) on this process delivers the rest. */
static struct sigaction sp_w32_sigs[SP_W32_NSIG];
static void sp_w32_veh_ensure(void);   /* the vectored handler, installed once (mmap below) */

/* The CRT's console and abort signals go through its signal(); the fault
   signals come from the vectored handler (sp_w32_fault_signal), which sees
   a fault on any stack -- the CRT's exception filter is reached only by an
   unwind that can walk back to mainCRTStartup, and one from a coroutine
   stack cannot. */
static int sp_w32_crt_signal(int sig) {
  return sig == SIGTERM || sig == SIGABRT;
}
/* the console's events, which arrive on a thread the system starts for
   them and are delivered to the main thread (sp_w32_async below) */
static int sp_w32_console_sig(int sig) { return sig == SIGINT || sig == SIGBREAK || sig == SIGHUP; }
static void sp_w32_console_arm(void);
static int sp_w32_fault_sig(int sig) { return sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE; }

/* A signal is blocked while its own handler runs (unless SA_NODEFER), and
   one that arrives meanwhile is pending and runs when the handler returns,
   as POSIX has it. A handler that leaves by a jump leaves it blocked until
   sigprocmask unblocks it -- the runtime does, before it raises. */
static volatile LONG sp_w32_sig_blocked[SP_W32_NSIG];
static volatile LONG sp_w32_sig_pending[SP_W32_NSIG];

static void sp_w32_call_handler(int sig) {
  struct sigaction *a = &sp_w32_sigs[sig];
  if (a->sa_flags & SA_SIGINFO) {
    siginfo_t si; memset(&si, 0, sizeof si);
    si.si_signo = sig; si.si_pid = (pid_t)GetCurrentProcessId();
    if (a->sa_sigaction) a->sa_sigaction(sig, &si, NULL);
  } else if (a->sa_handler && a->sa_handler != SIG_DFL && a->sa_handler != SIG_IGN) {
    a->sa_handler(sig);
  }
}

static void sp_w32_deliver(int sig) {
  if (sp_w32_sigs[sig].sa_flags & SA_NODEFER) { sp_w32_call_handler(sig); return; }
  if (InterlockedCompareExchange(&sp_w32_sig_blocked[sig], 1, 0) != 0) {
    InterlockedExchange(&sp_w32_sig_pending[sig], 1);
    return;
  }
  for (;;) {
    InterlockedExchange(&sp_w32_sig_pending[sig], 0);
    sp_w32_call_handler(sig);
    if (InterlockedExchange(&sp_w32_sig_pending[sig], 0)) continue;
    InterlockedExchange(&sp_w32_sig_blocked[sig], 0);
    /* one that came between the check and the unblock */
    if (sp_w32_sig_pending[sig] && InterlockedCompareExchange(&sp_w32_sig_blocked[sig], 1, 0) == 0) continue;
    return;
  }
}

static void __cdecl sp_w32_trampoline(int sig) {
  /* the CRT resets a handler as it delivers (SysV semantics); POSIX keeps it */
  if (!(sp_w32_sigs[sig].sa_flags & SA_RESETHAND)) signal(sig, sp_w32_trampoline);
  else { sp_w32_sigs[sig].sa_handler = SIG_DFL; sp_w32_sigs[sig].sa_flags = 0; }
  sp_w32_deliver(sig);
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
  if (sig <= 0 || sig >= SP_W32_NSIG || ((sig == SIGKILL || sig == SIGSTOP) && act)) { errno = EINVAL; return -1; }
  if (old) *old = sp_w32_sigs[sig];
  if (!act) return 0;
  sp_w32_sigs[sig] = *act;
  if (sp_w32_fault_sig(sig)) sp_w32_veh_ensure();
  if (sp_w32_console_sig(sig)) sp_w32_console_arm();
  if (sp_w32_crt_signal(sig)) {
    void (*h)(int) = act->sa_handler;
    if (!(act->sa_flags & SA_SIGINFO) && (h == SIG_DFL || h == SIG_IGN)) signal(sig, h);
    else signal(sig, sp_w32_trampoline);
  }
  return 0;
}

/* the calling thread's alternate signal stack, which a fault with
   SA_ONSTACK is delivered on (sp_w32_fault_signal) */
static __thread stack_t sp_w32_altstack = { NULL, SS_DISABLE, 0 };
static void sp_w32_stack_guarantee(void);
int sigaltstack(const stack_t *ss, stack_t *old) {
  if (old) *old = sp_w32_altstack;
  if (ss) sp_w32_stack_guarantee();   /* per thread, as the alternate stack is */
  if (ss) {
    if (!(ss->ss_flags & SS_DISABLE) && ss->ss_size < MINSIGSTKSZ) { errno = ENOMEM; return -1; }
    sp_w32_altstack = *ss;
    if (ss->ss_flags & SS_DISABLE) sp_w32_altstack.ss_sp = NULL;
  }
  return 0;
}
int sigemptyset(sigset_t *s) { if (!s) { errno = EINVAL; return -1; } *s = 0; return 0; }
int sigfillset(sigset_t *s) { if (!s) { errno = EINVAL; return -1; } *s = (sigset_t)~(sigset_t)0; return 0; }
int sigaddset(sigset_t *s, int sig) {
  if (!s || sig <= 0 || sig >= SP_W32_NSIG) { errno = EINVAL; return -1; }
  *s |= (sigset_t)1 << sig; return 0;
}
int sigdelset(sigset_t *s, int sig) {
  if (!s || sig <= 0 || sig >= SP_W32_NSIG) { errno = EINVAL; return -1; }
  *s &= ~((sigset_t)1 << sig); return 0;
}
int sigismember(const sigset_t *s, int sig) {
  if (!s || sig <= 0 || sig >= SP_W32_NSIG) { errno = EINVAL; return -1; }
  return (*s >> sig) & 1;
}
/* the mask is the blocked set above, process-wide (signals here are
   delivered synchronously or on a CRT thread of their own, not to a chosen
   thread); unblocking runs what was pending */
int sigprocmask(int how, const sigset_t *s, sigset_t *old) {
  if (old) {
    sigset_t o = 0;
    for (int i = 1; i < SP_W32_NSIG; i++) if (sp_w32_sig_blocked[i]) o |= (sigset_t)1 << i;
    *old = o;
  }
  if (!s) return 0;
  for (int i = 1; i < SP_W32_NSIG; i++) {
    int in = (int)((*s >> i) & 1);
    int want;
    if (how == SIG_BLOCK) { if (!in) continue; want = 1; }
    else if (how == SIG_UNBLOCK) { if (!in) continue; want = 0; }
    else if (how == SIG_SETMASK) want = in;
    else { errno = EINVAL; return -1; }
    if (want) InterlockedExchange(&sp_w32_sig_blocked[i], 1);
    else {
      InterlockedExchange(&sp_w32_sig_blocked[i], 0);
      if (InterlockedExchange(&sp_w32_sig_pending[i], 0)) sp_w32_deliver(i);
    }
  }
  return 0;
}
int sp_w32_pthread_sigmask(int how, const sigset_t *s, sigset_t *old) { return sigprocmask(how, s, old); }
int sigpending(sigset_t *s) { if (s) *s = 0; return 0; }
int sigsuspend(const sigset_t *s) { (void)s; while (SleepEx(INFINITE, TRUE) != WAIT_IO_COMPLETION) {} errno = EINTR; return -1; }

/* ---- asynchronous delivery ----
   A POSIX signal from outside -- the console's Ctrl-C, another thread's
   kill -- interrupts the main thread wherever it is and runs the handler
   there. Windows' special user APCs (Windows 11 and Server 2022) are that:
   queued to the main thread, they run on it at once, from user code or an
   alertable wait. Where they are missing the thread is suspended and its
   context pointed at the delivery, which then restores it. Waits the shim
   makes are alertable (nanosleep, pause), so a delivery cuts them short
   with EINTR as a signal does. */
static __thread volatile LONG sp_w32_apc_ran;

static void CALLBACK sp_w32_apc(ULONG_PTR sig) {
  sp_w32_apc_ran = 1;
  sp_w32_deliver((int)sig);
}

typedef struct { CONTEXT ctx; int sig; } sp_w32_hijack;
static void sp_w32_hijack_entry(sp_w32_hijack *h) {
  CONTEXT c = h->ctx;
  int sig = h->sig;
  free(h);
  sp_w32_apc_ran = 1;
  sp_w32_deliver(sig);
  RtlRestoreContext(&c, NULL);
}

static void sp_w32_async(int sig) {
  if (!sp_w32_main_thread) { sp_w32_deliver(sig); return; }
  if (GetCurrentThreadId() == sp_w32_main_tid) { sp_w32_deliver(sig); return; }
  if (sp_w32_qapc2 && sp_w32_qapc2(sp_w32_apc, sp_w32_main_thread, (ULONG_PTR)sig, 1 /* SPECIAL_USER_APC */)) return;
  sp_w32_hijack *h = (sp_w32_hijack *)malloc(sizeof *h);
  if (!h) return;
  if (SuspendThread(sp_w32_main_thread) == (DWORD)-1) { free(h); return; }
  h->ctx.ContextFlags = CONTEXT_FULL;
  if (!GetThreadContext(sp_w32_main_thread, &h->ctx)) { ResumeThread(sp_w32_main_thread); free(h); return; }
  h->sig = sig;
  CONTEXT c = h->ctx;
  /* below the interrupted frame (and any red zone), 16-aligned, with the
     return slot and home space a call would leave */
  DWORD64 sp = (c.Rsp - 256) & ~(DWORD64)15;
  sp -= 40;
  *(DWORD64 *)sp = 0;
  c.Rsp = sp;
  c.Rip = (DWORD64)(uintptr_t)sp_w32_hijack_entry;
  c.Rcx = (DWORD64)(uintptr_t)h;
  SetThreadContext(sp_w32_main_thread, &c);
  ResumeThread(sp_w32_main_thread);
}

static BOOL WINAPI sp_w32_ctrl(DWORD type) {
  int sig = type == CTRL_C_EVENT ? SIGINT : type == CTRL_BREAK_EVENT ? SIGBREAK :
            type == CTRL_CLOSE_EVENT ? SIGHUP : 0;
  if (!sig) return FALSE;
  struct sigaction *a = &sp_w32_sigs[sig];
  if (!(a->sa_flags & SA_SIGINFO) && a->sa_handler == SIG_IGN) return TRUE;
  if (!((a->sa_flags & SA_SIGINFO) ? a->sa_sigaction != NULL : (a->sa_handler && a->sa_handler != SIG_DFL)))
    return FALSE;   /* the default: the process ends */
  sp_w32_async(sig);
  return TRUE;
}

static void sp_w32_console_arm(void) {
  static volatile LONG armed = 0;
  if (InterlockedExchange(&armed, 1)) return;
  SetConsoleCtrlHandler(sp_w32_ctrl, TRUE);
}

/* nanosleep, interruptible: an alertable wait on a high-resolution timer
   (finer than Sleep's 15.6 ms tick), cut short by a delivery with EINTR
   and the time left */
int sp_w32_nanosleep(const struct timespec *req, struct timespec *rem) {
  if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L || req->tv_sec < 0) { errno = EINVAL; return -1; }
  static __thread HANDLE timer;
  if (!timer) {
    timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(NULL, TRUE, NULL);
  }
  LONGLONG due100 = (LONGLONG)req->tv_sec * 10000000LL + req->tv_nsec / 100;
  if (due100 <= 0) { SwitchToThread(); return 0; }
  LARGE_INTEGER t0, f; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&f);
  LARGE_INTEGER due; due.QuadPart = -due100;
  sp_w32_apc_ran = 0;
  DWORD w;
  if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) w = WaitForSingleObjectEx(timer, INFINITE, TRUE);
  else w = SleepEx((DWORD)((due100 + 9999) / 10000), TRUE) == WAIT_IO_COMPLETION ? WAIT_IO_COMPLETION : WAIT_OBJECT_0;
  if (w == WAIT_IO_COMPLETION || sp_w32_apc_ran) {
    if (rem) {
      LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
      LONGLONG el100 = (LONGLONG)((t1.QuadPart - t0.QuadPart) * 10000000.0 / (double)f.QuadPart);
      LONGLONG left = due100 > el100 ? due100 - el100 : 0;
      rem->tv_sec = (time_t)(left / 10000000LL);
      rem->tv_nsec = (long)((left % 10000000LL) * 100);
    }
    errno = EINTR;
    return -1;
  }
  if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
  return 0;
}

int kill(pid_t pid, int sig) {
  if (sig < 0 || sig >= SP_W32_NSIG) { errno = EINVAL; return -1; }
  DWORD me = GetCurrentProcessId();
  if (pid == 0 || pid == -1 || (DWORD)pid == me) {
    if (sig == 0) return 0;
    struct sigaction *a = &sp_w32_sigs[sig];
    if (a->sa_handler == SIG_IGN && !(a->sa_flags & SA_SIGINFO)) return 0;
    if ((a->sa_flags & SA_SIGINFO) ? a->sa_sigaction != NULL : (a->sa_handler && a->sa_handler != SIG_DFL)) {
      if (sp_w32_crt_signal(sig) && GetCurrentThreadId() == sp_w32_main_tid) return raise(sig);
      /* to the main thread, as a process-directed signal reaches it */
      sp_w32_async(sig);
      return 0;
    }
    /* the default action: these few are ignored, the rest end the process */
    if (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT) return 0;
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(), 128 + (UINT)sig);
    return 0;
  }
  if (pid < 0) pid = -pid;   /* a process group: its leader */
  AcquireSRWLockExclusive(&sp_w32_children_lock);
  int i = sp_w32_child_find((DWORD)pid);
  HANDLE h = NULL; int own = 0;
  if (i >= 0) h = sp_w32_children[i].h;
  ReleaseSRWLockExclusive(&sp_w32_children_lock);
  if (!h) {
    h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) { errno = GetLastError() == ERROR_ACCESS_DENIED ? EPERM : ESRCH; return -1; }
    own = 1;
  }
  DWORD code = 0;
  int alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  int r = 0;
  if (sig == 0) { if (!alive) { errno = ESRCH; r = -1; } }
  else if (!alive) { errno = ESRCH; r = -1; }
  else if (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT) { /* ignored by default */ }
  else if ((sig == SIGINT || sig == SIGBREAK) && GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, (DWORD)pid)) { }
  else {
    if (i >= 0) {
      AcquireSRWLockExclusive(&sp_w32_children_lock);
      int j = sp_w32_child_find((DWORD)pid);
      if (j >= 0) sp_w32_children[j].sig = sig;
      ReleaseSRWLockExclusive(&sp_w32_children_lock);
    }
    if (!TerminateProcess(h, 128 + (UINT)sig)) r = sp_w32_fail();
  }
  if (own) CloseHandle(h);
  return r;
}
int killpg(pid_t pgrp, int sig) { return kill(-pgrp, sig); }

const char *sp_w32_strsignal(int sig) {
  static const char *const names[SP_W32_NSIG] = {
    [SIGHUP] = "Hangup", [SIGINT] = "Interrupt", [SIGQUIT] = "Quit", [SIGILL] = "Illegal instruction",
    [SIGTRAP] = "Trace/breakpoint trap", [SIGBUS] = "Bus error", [SIGFPE] = "Floating point exception",
    [SIGKILL] = "Killed", [SIGUSR1] = "User defined signal 1", [SIGSEGV] = "Segmentation fault",
    [SIGUSR2] = "User defined signal 2", [SIGPIPE] = "Broken pipe", [SIGALRM] = "Alarm clock",
    [SIGTERM] = "Terminated", [SIGCHLD] = "Child exited", [SIGCONT] = "Continued",
    [SIGSTOP] = "Stopped (signal)", [SIGTSTP] = "Stopped", [SIGBREAK] = "Break", [SIGABRT] = "Aborted",
    [SIGTTIN] = "Stopped (tty input)", [SIGTTOU] = "Stopped (tty output)", [SIGURG] = "Urgent I/O condition",
    [SIGXCPU] = "CPU time limit exceeded", [SIGXFSZ] = "File size limit exceeded",
    [SIGVTALRM] = "Virtual timer expired", [SIGPROF] = "Profiling timer expired",
    [SIGWINCH] = "Window changed", [SIGIO] = "I/O possible", [SIGPWR] = "Power failure",
    [SIGSYS] = "Bad system call",
  };
  if (sig > 0 && sig < SP_W32_NSIG && names[sig]) return names[sig];
  return "Unknown signal";
}

/* ---- hardware faults as signals ----
   A fault the program's own code takes (an access violation the lazy-commit
   handler has no page for, an illegal instruction, a divide by zero) or a
   stack overflow anywhere is delivered to the sigaction handler for its
   signal, as the kernel delivers SIGSEGV on POSIX. A fault inside a system
   DLL is left alone: some catch their own (SEH), and those are not the
   program's. With SA_ONSTACK the handler runs on the thread's sigaltstack:
   the vectored handler points the faulting context at a trampoline on that
   stack and resumes into it. A stack overflow first gets its guard page
   back (the memory manager drops it as it raises the overflow), so the next
   one is caught too. A handler that returns resumes the fault as it was,
   which faults again -- with the handler reset to SIG_DFL, as the runtime's
   does, that ends the process the ordinary way. */
typedef struct {
  int sig;
  siginfo_t si;
  CONTEXT ctx;
  int overflow;
} sp_w32_fault;
static __thread sp_w32_fault sp_w32_cur_fault;

static int sp_w32_handler_set(int sig) {
  struct sigaction *a = &sp_w32_sigs[sig];
  if (a->sa_flags & SA_SIGINFO) return a->sa_sigaction != NULL;
  return a->sa_handler && a->sa_handler != SIG_DFL && a->sa_handler != SIG_IGN;
}

/* After an overflow, put the stack back the way a fresh one is laid out --
   the pages below the faulting one given back, one guard page under it, the
   TEB's limit there -- so the memory manager grows it again and raises the
   next overflow exactly as it raised this one. Everything below the fault
   is dead: the handler leaves by a jump to a frame far above it. Called on
   the alternate stack, before the handler runs. */
static void sp_w32_rearm_guard(char *sp) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(sp, &mbi, sizeof mbi)) return;
  SYSTEM_INFO si; GetSystemInfo(&si);
  uintptr_t pg = si.dwPageSize;
  char *bottom = (char *)mbi.AllocationBase;
  char *fault = (char *)((uintptr_t)sp & ~(pg - 1));
  if (fault - bottom < (ptrdiff_t)(2 * pg)) return;
  VirtualFree(bottom, (size_t)(fault - pg - bottom), MEM_DECOMMIT);
  if (!VirtualAlloc(fault - pg, pg, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD)) return;
  NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
  tib->StackLimit = fault;
}

/* the room a stack overflow is raised with on this thread, for the
   exception's own dispatch and the move to the alternate stack */
static void sp_w32_stack_guarantee(void) {
  ULONG g = 64 * 1024;
  SetThreadStackGuarantee(&g);
}

static void sp_w32_fault_run(sp_w32_fault *f) {
  struct sigaction *a = &sp_w32_sigs[f->sig];
  if (a->sa_flags & SA_SIGINFO) a->sa_sigaction(f->sig, &f->si, NULL);
  else a->sa_handler(f->sig);
}

/* entered on the alternate stack, with the fault in sp_w32_cur_fault */
static void sp_w32_fault_tramp(void) {
  sp_w32_fault *f = &sp_w32_cur_fault;
  if (f->overflow) sp_w32_rearm_guard((char *)f->ctx.Rsp);
  sp_w32_fault_run(f);
  RtlRestoreContext(&f->ctx, NULL);
}

static LONG sp_w32_fault_signal(PEXCEPTION_POINTERS ep) {
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  int sig = 0, overflow = 0;
  void *addr = ep->ExceptionRecord->ExceptionAddress;
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
      sig = SIGSEGV;
      if (ep->ExceptionRecord->NumberParameters >= 2) addr = (void *)ep->ExceptionRecord->ExceptionInformation[1];
      break;
    case EXCEPTION_STACK_OVERFLOW:
      /* the address is where the stack ran out: the stack pointer, just
         above the stack's reserved bottom */
      sig = SIGSEGV; overflow = 1; addr = (void *)ep->ContextRecord->Rsp;
      break;
    case EXCEPTION_IN_PAGE_ERROR: sig = SIGBUS; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION: sig = SIGILL; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_INT_OVERFLOW: sig = SIGFPE; break;
    default: return EXCEPTION_CONTINUE_SEARCH;
  }
  if (!sp_w32_handler_set(sig)) return EXCEPTION_CONTINUE_SEARCH;
  if (!overflow) {
    HMODULE mod = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &mod) ||
        mod != GetModuleHandleW(NULL)) return EXCEPTION_CONTINUE_SEARCH;
  }
  sp_w32_fault *f = &sp_w32_cur_fault;
  memset(&f->si, 0, sizeof f->si);
  f->sig = sig;
  f->si.si_signo = sig;
  f->si.si_addr = addr;
  f->overflow = overflow;
  f->ctx = *ep->ContextRecord;
  struct sigaction *a = &sp_w32_sigs[sig];
  if ((a->sa_flags & SA_ONSTACK) && sp_w32_altstack.ss_sp && !(sp_w32_altstack.ss_flags & SS_DISABLE)) {
    /* resume on the alternate stack, in the trampoline: entry as after a
       call (rsp % 16 == 8), its home space above the return slot */
    uintptr_t top = ((uintptr_t)sp_w32_altstack.ss_sp + sp_w32_altstack.ss_size) & ~(uintptr_t)15;
    ep->ContextRecord->Rsp = (DWORD64)(top - 40);
    *(void **)(top - 40) = NULL;
    ep->ContextRecord->Rip = (DWORD64)(uintptr_t)sp_w32_fault_tramp;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (overflow) return EXCEPTION_CONTINUE_SEARCH;   /* no stack to run a handler on */
  sp_w32_fault_run(f);
  return EXCEPTION_CONTINUE_EXECUTION;   /* the handler returned: fault again */
}

unsigned int alarm(unsigned int seconds) { (void)seconds; return 0; }
int pause(void) { while (SleepEx(INFINITE, TRUE) != WAIT_IO_COMPLETION) {} errno = EINTR; return -1; }

/* ===================================================================== */
/* mmap                                                                  */
/* ===================================================================== */

/* Each mapping the shim made, by address range. An anonymous one is
   reserved and committed in granules as it is first touched (the vectored
   handler below); `lo`/`hi` are the part still mapped (munmap of a head or
   a tail moves them: Windows releases a reservation only whole), and the
   PROT_NONE ranges are pages the handler must leave alone so a touch there
   faults as it would on POSIX. */
typedef struct { char *lo, *hi; } sp_w32_range;
typedef struct {
  char *base; size_t size;
  char *lo, *hi;
  int lazy, file;
  DWORD prot;
  sp_w32_range *none; int nnone, capnone;
} sp_w32_map;
static sp_w32_map *sp_w32_maps;
static int sp_w32_nmaps, sp_w32_capmaps;
static SRWLOCK sp_w32_maps_lock = SRWLOCK_INIT;
static PVOID sp_w32_veh;

static DWORD sp_w32_page_prot(int prot) {
  if (prot & PROT_EXEC) return (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : (prot & PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
  if (prot & PROT_WRITE) return PAGE_READWRITE;
  if (prot & PROT_READ) return PAGE_READONLY;
  return PAGE_NOACCESS;
}

static int sp_w32_map_find(const char *p) {
  int lo = 0, hi = sp_w32_nmaps - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    sp_w32_map *m = &sp_w32_maps[mid];
    if (p < m->base) hi = mid - 1;
    else if (p >= m->base + m->size) lo = mid + 1;
    else return mid;
  }
  return -1;
}

static int sp_w32_in_none(sp_w32_map *m, char *p) {
  for (int i = 0; i < m->nnone; i++) if (p >= m->none[i].lo && p < m->none[i].hi) return 1;
  return 0;
}

static LONG sp_w32_fault_signal(PEXCEPTION_POINTERS ep);

/* commit the granule around a first touch, clipped to the mapped part and
   short of any PROT_NONE page; any other fault is a signal (see below) */
static LONG CALLBACK sp_w32_veh_handler(PEXCEPTION_POINTERS ep) {
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
      ep->ExceptionRecord->NumberParameters < 2) return sp_w32_fault_signal(ep);
  char *addr = (char *)ep->ExceptionRecord->ExceptionInformation[1];
  LONG res = EXCEPTION_CONTINUE_SEARCH;
  AcquireSRWLockShared(&sp_w32_maps_lock);
  int i = sp_w32_map_find(addr);
  if (i >= 0) {
    sp_w32_map *m = &sp_w32_maps[i];
    if (m->lazy && addr >= m->lo && addr < m->hi && !sp_w32_in_none(m, addr)) {
      MEMORY_BASIC_INFORMATION mbi;
      if (VirtualQuery(addr, &mbi, sizeof mbi) && mbi.State == MEM_RESERVE) {
        size_t gran = m->size >= ((size_t)64 << 20) ? ((size_t)1 << 20) : ((size_t)64 << 10);
        char *g0 = m->base + ((size_t)(addr - m->base) / gran) * gran;
        char *g1 = g0 + gran;
        if (g0 < m->lo) g0 = m->lo;
        if (g1 > m->hi) g1 = m->hi;
        /* stay inside the reserved run the fault is in, and off PROT_NONE pages */
        char *r0 = (char *)mbi.BaseAddress, *r1 = r0 + mbi.RegionSize;
        if (g0 < r0) g0 = r0;
        if (g1 > r1) g1 = r1;
        for (int k = 0; k < m->nnone; k++) {
          sp_w32_range *n = &m->none[k];
          if (n->hi <= g0 || n->lo >= g1) continue;
          if (addr < n->lo) g1 = n->lo; else g0 = n->hi;
        }
        if (g0 <= addr && addr < g1 && VirtualAlloc(g0, (size_t)(g1 - g0), MEM_COMMIT, m->prot))
          res = EXCEPTION_CONTINUE_EXECUTION;
      } else if (VirtualQuery(addr, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && mbi.Protect != PAGE_NOACCESS) {
        res = EXCEPTION_CONTINUE_EXECUTION;   /* another thread committed it first */
      }
    }
  }
  ReleaseSRWLockShared(&sp_w32_maps_lock);
  if (res == EXCEPTION_CONTINUE_SEARCH) return sp_w32_fault_signal(ep);
  return res;
}

static void sp_w32_veh_install(void) {
  if (!sp_w32_veh) sp_w32_veh = AddVectoredExceptionHandler(1, sp_w32_veh_handler);
}

static void sp_w32_veh_ensure(void) {
  AcquireSRWLockExclusive(&sp_w32_maps_lock);
  sp_w32_veh_install();
  ReleaseSRWLockExclusive(&sp_w32_maps_lock);
}

static void sp_w32_map_add(sp_w32_map *m) {
  AcquireSRWLockExclusive(&sp_w32_maps_lock);
  sp_w32_veh_install();
  if (sp_w32_nmaps == sp_w32_capmaps) {
    int nc = sp_w32_capmaps ? sp_w32_capmaps * 2 : 32;
    sp_w32_map *nx = (sp_w32_map *)realloc(sp_w32_maps, sizeof *nx * (size_t)nc);
    if (!nx) { ReleaseSRWLockExclusive(&sp_w32_maps_lock); return; }
    sp_w32_maps = nx; sp_w32_capmaps = nc;
  }
  int at = sp_w32_nmaps;
  while (at > 0 && sp_w32_maps[at - 1].base > m->base) { sp_w32_maps[at] = sp_w32_maps[at - 1]; at--; }
  sp_w32_maps[at] = *m;
  sp_w32_nmaps++;
  ReleaseSRWLockExclusive(&sp_w32_maps_lock);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long long off) {
  if (len == 0) { errno = EINVAL; return MAP_FAILED; }
  sp_w32_map m; memset(&m, 0, sizeof m);
  if (flags & MAP_ANONYMOUS) {
    if ((flags & MAP_FIXED) && addr) { errno = ENOTSUP; return MAP_FAILED; }
    /* A stack is mapped the way Windows maps a thread's: reserved, its top
       committed, a guard page below. The memory manager grows it a page at
       a time from there once the TEB names it as the running stack (the
       context switch does, see sp_win32_ctx.c) -- a first touch cannot go
       through the handler below, which would need stack to run on. */
    if (flags & MAP_STACK) {
      SYSTEM_INFO si; GetSystemInfo(&si);
      size_t pg = si.dwPageSize, top = (size_t)64 << 10;
      len = (len + pg - 1) & ~(pg - 1);
      if (top + pg > len) top = len > pg ? len - pg : len;
      char *p = (char *)VirtualAlloc(NULL, len, MEM_RESERVE, PAGE_NOACCESS);
      if (!p) { errno = ENOMEM; return MAP_FAILED; }
      if (!VirtualAlloc(p + len - top, top, MEM_COMMIT, PAGE_READWRITE) ||
          (len > top && !VirtualAlloc(p + len - top - pg, pg, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD))) {
        VirtualFree(p, 0, MEM_RELEASE); errno = ENOMEM; return MAP_FAILED;
      }
      m.base = p; m.size = len; m.lo = p; m.hi = p + len;
      m.lazy = 0; m.prot = PAGE_READWRITE;
      sp_w32_map_add(&m);
      return p;
    }
    /* a reservation and lazy commits, unless the mapping is small and wanted
       now: the commit charge of an untouched page is all a lazy one saves */
    int lazy = (flags & MAP_NORESERVE) || len >= ((size_t)1 << 20) || prot == PROT_NONE;
    DWORD pp = sp_w32_page_prot(prot);
    void *p = VirtualAlloc(NULL, len, lazy ? MEM_RESERVE : (MEM_RESERVE | MEM_COMMIT), lazy ? PAGE_NOACCESS : pp);
    if (!p) { errno = ENOMEM; return MAP_FAILED; }
    m.base = (char *)p; m.size = len; m.lo = m.base; m.hi = m.base + len;
    m.lazy = lazy; m.prot = prot == PROT_NONE ? PAGE_READWRITE : pp;
    if (prot == PROT_NONE && lazy) {
      m.none = (sp_w32_range *)malloc(sizeof *m.none * 4);
      if (m.none) { m.none[0].lo = m.lo; m.none[0].hi = m.hi; m.nnone = 1; m.capnone = 4; }
    }
    sp_w32_map_add(&m);
    return p;
  }
  /* a file view */
  HANDLE fh = (HANDLE)_get_osfhandle(fd);
  if (fh == INVALID_HANDLE_VALUE) { errno = EBADF; return MAP_FAILED; }
  int priv = (flags & MAP_PRIVATE) != 0;
  DWORD fprot = (prot & PROT_WRITE) ? (priv ? PAGE_WRITECOPY : PAGE_READWRITE) : PAGE_READONLY;
  if (prot & PROT_EXEC) fprot = (prot & PROT_WRITE) ? (priv ? PAGE_EXECUTE_WRITECOPY : PAGE_EXECUTE_READWRITE) : PAGE_EXECUTE_READ;
  unsigned long long end = (unsigned long long)off + len;
  HANDLE mh = CreateFileMappingW(fh, NULL, fprot, (DWORD)(end >> 32), (DWORD)end, NULL);
  if (!mh) { sp_w32_fail(); return MAP_FAILED; }
  DWORD acc = (prot & PROT_WRITE) ? (priv ? FILE_MAP_COPY : FILE_MAP_WRITE) : FILE_MAP_READ;
  if (prot & PROT_EXEC) acc |= FILE_MAP_EXECUTE;
  void *p = MapViewOfFile(mh, acc, (DWORD)((unsigned long long)off >> 32), (DWORD)off, len);
  DWORD err = GetLastError();
  CloseHandle(mh);
  if (!p) { errno = sp_w32_errno_of(err); return MAP_FAILED; }
  m.base = (char *)p; m.size = len; m.lo = m.base; m.hi = m.base + len; m.file = 1;
  sp_w32_map_add(&m);
  return p;
}

int munmap(void *addr, size_t len) {
  char *a = (char *)addr, *e = a + len;
  AcquireSRWLockExclusive(&sp_w32_maps_lock);
  int i = sp_w32_map_find(a);
  if (i < 0) { ReleaseSRWLockExclusive(&sp_w32_maps_lock); errno = EINVAL; return -1; }
  sp_w32_map *m = &sp_w32_maps[i];
  if (e > m->base + m->size) e = m->base + m->size;
  int whole = a <= m->lo && e >= m->hi;
  if (!whole && !m->file) {
    /* a head, a tail or a hole: give the pages back, keep the reservation */
    VirtualFree(a, (size_t)(e - a), MEM_DECOMMIT);
    if (a <= m->lo) m->lo = e;
    else if (e >= m->hi) m->hi = a;
    else if (m->lazy) {
      if (m->nnone == m->capnone) {
        int nc = m->capnone ? m->capnone * 2 : 4;
        sp_w32_range *nx = (sp_w32_range *)realloc(m->none, sizeof *nx * (size_t)nc);
        if (nx) { m->none = nx; m->capnone = nc; }
      }
      if (m->nnone < m->capnone) { m->none[m->nnone].lo = a; m->none[m->nnone].hi = e; m->nnone++; }
    }
    ReleaseSRWLockExclusive(&sp_w32_maps_lock);
    return 0;
  }
  if (m->file) UnmapViewOfFile(m->base);
  else VirtualFree(m->base, 0, MEM_RELEASE);
  free(m->none);
  for (int k = i; k + 1 < sp_w32_nmaps; k++) sp_w32_maps[k] = sp_w32_maps[k + 1];
  sp_w32_nmaps--;
  ReleaseSRWLockExclusive(&sp_w32_maps_lock);
  return 0;
}

int mprotect(void *addr, size_t len, int prot) {
  char *a = (char *)addr, *e = a + len;
  AcquireSRWLockExclusive(&sp_w32_maps_lock);
  int i = sp_w32_map_find(a);
  sp_w32_map *m = i >= 0 ? &sp_w32_maps[i] : NULL;
  int r = 0;
  if (m && m->lazy) {
    /* drop the range from the PROT_NONE set, or add it */
    int k = 0;
    while (k < m->nnone) {
      sp_w32_range *n = &m->none[k];
      if (n->lo >= a && n->hi <= e) { m->none[k] = m->none[--m->nnone]; continue; }
      k++;
    }
    if (prot == PROT_NONE) {
      if (m->nnone == m->capnone) {
        int nc = m->capnone ? m->capnone * 2 : 4;
        sp_w32_range *nx = (sp_w32_range *)realloc(m->none, sizeof *nx * (size_t)nc);
        if (nx) { m->none = nx; m->capnone = nc; }
      }
      if (m->nnone < m->capnone) { m->none[m->nnone].lo = a; m->none[m->nnone].hi = e; m->nnone++; }
    }
    /* the pages already committed take the protection now */
    char *p = a;
    while (p < e) {
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
      char *q = (char *)mbi.BaseAddress + mbi.RegionSize;
      if (q > e) q = e;
      if (mbi.State == MEM_COMMIT) {
        DWORD old;
        VirtualProtect(p, (size_t)(q - p), prot == PROT_NONE ? PAGE_NOACCESS : sp_w32_page_prot(prot), &old);
      }
      p = q;
    }
    if (prot != PROT_NONE) m->prot = sp_w32_page_prot(prot);
  } else if (m) {
    /* the committed runs take it; a reserved run (below a stack's guard
       page) faults on a touch already, which is what PROT_NONE asks */
    char *p = a;
    while (p < e) {
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery(p, &mbi, sizeof mbi)) { r = sp_w32_fail(); break; }
      char *q = (char *)mbi.BaseAddress + mbi.RegionSize;
      if (q > e) q = e;
      DWORD old;
      if (mbi.State == MEM_COMMIT && !VirtualProtect(p, (size_t)(q - p), sp_w32_page_prot(prot), &old)) { r = sp_w32_fail(); break; }
      p = q;
    }
  } else {
    DWORD old;
    if (!VirtualProtect(a, len, sp_w32_page_prot(prot), &old)) r = sp_w32_fail();
  }
  ReleaseSRWLockExclusive(&sp_w32_maps_lock);
  return r;
}

int madvise(void *addr, size_t len, int advice) {
  if (advice != MADV_DONTNEED && advice != MADV_FREE) return 0;
  SYSTEM_INFO si; GetSystemInfo(&si);
  uintptr_t pg = si.dwPageSize;
  char *a = (char *)(((uintptr_t)addr + pg - 1) & ~(pg - 1));
  char *e = (char *)(((uintptr_t)addr + len) & ~(pg - 1));
  if (e <= a) return 0;
  AcquireSRWLockShared(&sp_w32_maps_lock);
  int i = sp_w32_map_find(a);
  int lazy = i >= 0 && sp_w32_maps[i].lazy && !sp_w32_maps[i].file;
  DWORD prot = i >= 0 ? sp_w32_maps[i].prot : PAGE_READWRITE;
  ReleaseSRWLockShared(&sp_w32_maps_lock);
  if (i < 0) return 0;
  if (advice == MADV_FREE) { VirtualAlloc(a, (size_t)(e - a), MEM_RESET, PAGE_NOACCESS); return 0; }
  /* DONTNEED: the pages go back and the next read sees zeros, as Linux
     promises for a private anonymous mapping. Decommitted and committed
     again at once: a committed page no one has touched costs no memory and
     reads zero, and leaving it decommitted would send the next touch
     through the lazy-commit handler -- a user-mode exception per page,
     which a collector that hands chunks back every cycle paid thousands of
     times a second. */
  (void)lazy;
  VirtualFree(a, (size_t)(e - a), MEM_DECOMMIT);
  VirtualAlloc(a, (size_t)(e - a), MEM_COMMIT, prot);
  return 0;
}

int msync(void *addr, size_t len, int flags) {
  (void)flags;
  return FlushViewOfFile(addr, len) ? 0 : sp_w32_fail();
}
int mlock(const void *addr, size_t len) { return VirtualLock((LPVOID)addr, len) ? 0 : sp_w32_fail(); }
int munlock(const void *addr, size_t len) { return VirtualUnlock((LPVOID)addr, len) ? 0 : sp_w32_fail(); }

/* ===================================================================== */
/* fnmatch                                                               */
/* ===================================================================== */

static int sp_w32_fold(int c, int flags) { return (flags & FNM_CASEFOLD) ? tolower((unsigned char)c) : (unsigned char)c; }

static int sp_w32_class_match(const char *name, size_t n, int c) {
#define SP_W32_CLS(s, f) if (n == sizeof(s) - 1 && strncmp(name, s, n) == 0) return f(c) != 0;
  SP_W32_CLS("alpha", isalpha) SP_W32_CLS("digit", isdigit) SP_W32_CLS("alnum", isalnum)
  SP_W32_CLS("upper", isupper) SP_W32_CLS("lower", islower) SP_W32_CLS("space", isspace)
  SP_W32_CLS("punct", ispunct) SP_W32_CLS("print", isprint) SP_W32_CLS("graph", isgraph)
  SP_W32_CLS("cntrl", iscntrl) SP_W32_CLS("xdigit", isxdigit) SP_W32_CLS("blank", isblank)
#undef SP_W32_CLS
  return 0;
}

/* a bracket expression at *pp against c: 1 match, 0 no match, -1 not a
   bracket (the '[' is then an ordinary character) */
static int sp_w32_bracket(const char **pp, int c, int flags) {
  const char *p = *pp + 1;
  int neg = 0, ok = 0;
  if (*p == '!' || *p == '^') { neg = 1; p++; }
  const char *start = p;
  int fc = sp_w32_fold(c, flags);
  while (*p && (*p != ']' || p == start)) {
    if (*p == '[' && p[1] == ':') {
      const char *e = strstr(p + 2, ":]");
      if (e) { if (sp_w32_class_match(p + 2, (size_t)(e - p - 2), c)) ok = 1; p = e + 2; continue; }
    }
    int lo = (unsigned char)*p;
    if (lo == '\\' && !(flags & FNM_NOESCAPE) && p[1]) { p++; lo = (unsigned char)*p; }
    p++;
    if (*p == '-' && p[1] && p[1] != ']') {
      int hi = (unsigned char)p[1];
      if (hi == '\\' && !(flags & FNM_NOESCAPE) && p[2]) { p++; hi = (unsigned char)p[1]; }
      p += 2;
      if ((flags & FNM_CASEFOLD) ? (fc >= tolower(lo) && fc <= tolower(hi)) || (c >= lo && c <= hi) : (c >= lo && c <= hi)) ok = 1;
    } else if (sp_w32_fold(lo, flags) == fc) ok = 1;
  }
  if (*p != ']') return -1;
  *pp = p + 1;
  return ok != neg;
}

static int sp_w32_fnmatch(const char *p, const char *s, const char *s0, int flags) {
  for (;;) {
    int c = (unsigned char)*p;
    if (c == 0) return (*s == 0 || ((flags & FNM_LEADING_DIR) && *s == '/')) ? 0 : FNM_NOMATCH;
    /* a leading period matches only a period */
    if ((flags & FNM_PERIOD) && *s == '.' && (s == s0 || ((flags & FNM_PATHNAME) && s[-1] == '/')) && c != '.')
      return FNM_NOMATCH;
    switch (c) {
      case '?':
        if (!*s || ((flags & FNM_PATHNAME) && *s == '/')) return FNM_NOMATCH;
        p++; s++; break;
      case '*': {
        while (*p == '*') p++;
        if (!*p) {
          if (flags & FNM_PATHNAME) return ((flags & FNM_LEADING_DIR) || !strchr(s, '/')) ? 0 : FNM_NOMATCH;
          return 0;
        }
        for (const char *t = s; *t; t++) {
          if (sp_w32_fnmatch(p, t, s0, flags & ~FNM_PERIOD) == 0) return 0;
          if ((flags & FNM_PATHNAME) && *t == '/') break;
        }
        if ((flags & FNM_PATHNAME) && *p == '/') {
          const char *sl = strchr(s, '/');
          if (sl) return sp_w32_fnmatch(p, sl, s0, flags);
        }
        return FNM_NOMATCH;
      }
      case '[': {
        if (!*s || ((flags & FNM_PATHNAME) && *s == '/')) return FNM_NOMATCH;
        const char *q = p;
        int r = sp_w32_bracket(&q, (unsigned char)*s, flags);
        if (r < 0) { if (*s != '[') return FNM_NOMATCH; p++; s++; break; }
        if (!r) return FNM_NOMATCH;
        p = q; s++; break;
      }
      case '\\':
        if (!(flags & FNM_NOESCAPE) && p[1]) p++;
        /* fall through */
      default:
        if (sp_w32_fold((unsigned char)*p, flags) != sp_w32_fold((unsigned char)*s, flags)) return FNM_NOMATCH;
        p++; s++; break;
    }
  }
}

int fnmatch(const char *pattern, const char *string, int flags) {
  if (!pattern || !string) return FNM_NOMATCH;
  return sp_w32_fnmatch(pattern, string, string, flags);
}

/* ===================================================================== */
/* dlopen                                                                */
/* ===================================================================== */

static char sp_w32_dlerr[512];
static int sp_w32_dlerr_set;
static void sp_w32_dl_fail(const char *what) {
  DWORD e = GetLastError();
  char msg[400] = "";
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, msg, sizeof msg, NULL);
  size_t n = strlen(msg);
  while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r' || msg[n - 1] == ' ')) msg[--n] = 0;
  snprintf(sp_w32_dlerr, sizeof sp_w32_dlerr, "%s: %s", what ? what : "dlopen", msg);
  sp_w32_dlerr_set = 1;
}

void *dlopen(const char *file, int mode) {
  (void)mode;
  if (!file) return GetModuleHandleW(NULL);
  wchar_t wb[SP_W32_WBUF], *w = sp_w32_wpath(file, wb, SP_W32_WBUF);
  if (!w) { sp_w32_dl_fail(file); return NULL; }
  for (wchar_t *c = w; *c; c++) if (*c == L'/') *c = L'\\';
  HMODULE h = LoadLibraryExW(w, NULL, wcschr(w, L'\\') ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
  sp_w32_wfree(w, wb);
  if (!h) sp_w32_dl_fail(file);
  return (void *)h;
}

void *dlsym(void *handle, const char *name) {
  if (handle && handle != GetModuleHandleW(NULL)) {
    FARPROC p = GetProcAddress((HMODULE)handle, name);
    if (!p) sp_w32_dl_fail(name);
    return (void *)p;
  }
  /* the global namespace: every module loaded, the program first */
  HMODULE mods[512]; DWORD need = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) { sp_w32_dl_fail(name); return NULL; }
  DWORD n = need / sizeof(HMODULE);
  if (n > 512) n = 512;
  for (DWORD i = 0; i < n; i++) {
    FARPROC p = GetProcAddress(mods[i], name);
    if (p) return (void *)p;
  }
  SetLastError(ERROR_PROC_NOT_FOUND);
  sp_w32_dl_fail(name);
  return NULL;
}

int dlclose(void *handle) {
  if (!handle || handle == GetModuleHandleW(NULL)) return 0;
  return FreeLibrary((HMODULE)handle) ? 0 : (sp_w32_dl_fail("dlclose"), -1);
}

char *dlerror(void) {
  if (!sp_w32_dlerr_set) return NULL;
  sp_w32_dlerr_set = 0;
  return sp_w32_dlerr;
}

/* ===================================================================== */
/* locales                                                               */
/* ===================================================================== */

/* The only locale the runtime asks for is "C", for float formatting, and
   the UCRT formats in the "C" locale until a program calls setlocale; a
   token answers newlocale and uselocale has nothing to switch. */
static struct sp_w32_locale { int dummy; } sp_w32_c_locale;
locale_t newlocale(int mask, const char *name, locale_t base) { (void)mask; (void)name; (void)base; return &sp_w32_c_locale; }
locale_t uselocale(locale_t loc) { (void)loc; return LC_GLOBAL_LOCALE; }
void freelocale(locale_t loc) { (void)loc; }
locale_t duplocale(locale_t loc) { return loc; }

/* ===================================================================== */
/* threads                                                               */
/* ===================================================================== */

/* the stack bounds of the calling thread, the one thread pthread_getattr_np
   is ever asked about here; pthread_attr_getstack reads them back */
static __thread void *sp_w32_stack_lo;
static __thread size_t sp_w32_stack_size;
int pthread_getattr_np(pthread_t t, pthread_attr_t *a) {
  (void)t;
  if (pthread_attr_init(a) != 0) return ENOMEM;
  ULONG_PTR lo = 0, hi = 0;
  GetCurrentThreadStackLimits(&lo, &hi);
  sp_w32_stack_lo = (void *)lo;
  sp_w32_stack_size = (size_t)(hi - lo);
  return 0;
}
int sp_w32_pthread_attr_getstack(const pthread_attr_t *a, void **addr, size_t *size) {
  (void)a;
  *addr = sp_w32_stack_lo;
  *size = sp_w32_stack_size;
  return 0;
}

#endif /* _WIN32 */

/* ===================================================================== */
/* strftime                                                              */
/* ===================================================================== */
#ifdef _WIN32
#undef strftime
#undef gmtime_r
#undef localtime_r
#undef gmtime
#undef localtime
#undef mktime
#undef timegm

/* days since 1970-01-01 of a proleptic Gregorian date, and back (Howard
   Hinnant's algorithms), for any year a 64-bit time_t can name */
static long long sp_w32_days_from_civil(long long y, unsigned m, unsigned d) {
  y -= m <= 2;
  long long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long long)doe - 719468;
}
static void sp_w32_civil_from_days(long long z, long long *y, unsigned *m, unsigned *d) {
  z += 719468;
  long long era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long long yy = (long long)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = yy + (*m <= 2);
}

struct tm *sp_w32_gmtime_r(const time_t *t, struct tm *o) {
  if (!t || !o) { errno = EINVAL; return NULL; }
  long long s = (long long)*t;
  long long days = s / 86400, rem = s % 86400;
  if (rem < 0) { rem += 86400; days--; }
  long long y; unsigned m, d;
  sp_w32_civil_from_days(days, &y, &m, &d);
  if (y - 1900 > 0x7fffffffLL || y - 1900 < -0x7fffffffLL) { errno = EOVERFLOW; return NULL; }
  memset(o, 0, sizeof *o);
  o->tm_year = (int)(y - 1900);
  o->tm_mon = (int)m - 1;
  o->tm_mday = (int)d;
  o->tm_hour = (int)(rem / 3600);
  o->tm_min = (int)(rem / 60 % 60);
  o->tm_sec = (int)(rem % 60);
  o->tm_wday = (int)(((days % 7) + 11) % 7);   /* 1970-01-01 was a Thursday */
  o->tm_yday = (int)(days - sp_w32_days_from_civil(y, 1, 1));
  o->tm_isdst = 0;
  return o;
}

/* tzset(3). A POSIX TZ string is "std offset [dst [offset]][,rule]", the
   offsets hours west of UTC; the DST one defaults to an hour less than the
   standard one. The UCRT's _tzset reads the names and the standard offset,
   applies its US rule for DST, and keeps whatever DST bias it had: set it
   from the string. (A rule after the names is the UCRT's to apply.) */
static int sp_w32_tz_name(const char **p) {
  const char *s = *p;
  if (*s == '<') { const char *e = strchr(s, '>'); if (!e) return 0; *p = e + 1; return 1; }
  while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) s++;
  if (s - *p < 3) return 0;
  *p = s;
  return 1;
}
static int sp_w32_tz_off(const char **p, long *out) {
  const char *s = *p;
  long sign = 1, h, m = 0, sec = 0;
  if (*s == '+' || *s == '-') { if (*s == '-') sign = -1; s++; }
  if (*s < '0' || *s > '9') return 0;
  h = strtol(s, (char **)&s, 10);
  if (*s == ':') { m = strtol(s + 1, (char **)&s, 10); if (*s == ':') sec = strtol(s + 1, (char **)&s, 10); }
  *out = sign * (h * 3600 + m * 60 + sec);
  *p = s;
  return 1;
}
void sp_w32_tzset(void) {
  _tzset();
  const char *tz = getenv("TZ");
  int daylight = 0;
  _get_daylight(&daylight);
  if (!tz || !*tz || *tz == ':' || !daylight) return;
  const char *p = tz;
  long std_off, dst_off;
  if (!sp_w32_tz_name(&p) || !sp_w32_tz_off(&p, &std_off) || !sp_w32_tz_name(&p)) return;
  _dstbias = sp_w32_tz_off(&p, &dst_off) ? dst_off - std_off : -3600L;
}

/* the UCRT's own range for its local-time conversions */
#define SP_W32_UCRT_TMAX 32535215999LL   /* 3000-12-31 23:59:59 UTC */

struct tm *sp_w32_localtime_r(const time_t *t, struct tm *o) {
  if (!t || !o) { errno = EINVAL; return NULL; }
  if (*t >= 0 && *t <= SP_W32_UCRT_TMAX && _localtime64_s(o, (const __time64_t *)t) == 0) return o;
  long tz = 0; _get_timezone(&tz);
  time_t sh = *t - tz;
  return sp_w32_gmtime_r(&sh, o);
}

static __thread struct tm sp_w32_tm_buf;
struct tm *sp_w32_gmtime(const time_t *t) { return sp_w32_gmtime_r(t, &sp_w32_tm_buf); }
struct tm *sp_w32_localtime(const time_t *t) { return sp_w32_localtime_r(t, &sp_w32_tm_buf); }

/* timegm(3): the fields read as UTC, normalized as mktime normalizes them */
time_t sp_w32_timegm(struct tm *tm) {
  long long mon = tm->tm_mon, year = (long long)tm->tm_year + 1900;
  year += mon / 12; mon %= 12;
  if (mon < 0) { mon += 12; year--; }
  long long days = sp_w32_days_from_civil(year, (unsigned)mon + 1, 1) + (long long)tm->tm_mday - 1;
  long long s = days * 86400 + (long long)tm->tm_hour * 3600 + (long long)tm->tm_min * 60 + tm->tm_sec;
  time_t r = (time_t)s;
  sp_w32_gmtime_r(&r, tm);
  return r;
}

time_t sp_w32_mktime(struct tm *tm) {
  struct tm c = *tm;
  long long y = (long long)tm->tm_year + 1900;
  if (y >= 1970 && y <= 3000) {
    time_t r = _mktime64(&c);
    if (r != (time_t)-1) { *tm = c; return r; }
  }
  /* outside the UCRT's range: the zone's standard offset */
  long tz = 0; _get_timezone(&tz);
  c = *tm;
  time_t u = sp_w32_timegm(&c);
  time_t r = u + tz;
  sp_w32_localtime_r(&r, tm);
  return r;
}

/* the zone's abbreviation: TZ's own when it names one, else the initials
   of the Windows zone name ("Coordinated Universal Time" is UTC) */
static void sp_w32_zone_abbr(int isdst, char *out, size_t cap) {
  const char *n = _tzname[isdst > 0 ? 1 : 0];
  if (!n || !*n) { snprintf(out, cap, "%s", ""); return; }
  if (!strchr(n, ' ')) { snprintf(out, cap, "%s", n); return; }
  if (strstr(n, "Coordinated Universal") || strstr(n, "UTC")) { snprintf(out, cap, "UTC"); return; }
  size_t k = 0;
  for (const char *p = n; *p && k + 1 < cap; p++)
    if ((p == n || p[-1] == ' ') && isalpha((unsigned char)*p)) out[k++] = (char)toupper((unsigned char)*p);
  out[k] = 0;
}

/* ISO 8601 week-based year and week number */
static int sp_w32_iso_week(const struct tm *t, int *year) {
  int y = t->tm_year + 1900, yday = t->tm_yday, wday = (t->tm_wday + 6) % 7;   /* Monday = 0 */
  int w = (yday - wday + 10) / 7;
  if (w < 1) {
    y--;
    int py = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    w = (yday + py - wday + 10) / 7;
  } else {
    int ylen = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    if (yday - wday + 3 >= ylen) { w = 1; y++; }   /* the Thursday is next year's */
  }
  if (year) *year = y;
  return w;
}

size_t sp_w32_strftime(char *buf, size_t cap, const char *fmt, const struct tm *t) {
  static const char *const wd[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
  static const char *const mo[] = { "January", "February", "March", "April", "May", "June", "July",
                                    "August", "September", "October", "November", "December" };
  size_t k = 0;
#define SP_W32_PUTS(s) do { const char *_s = (s); size_t _n = strlen(_s); if (k + _n >= cap) return 0; memcpy(buf + k, _s, _n); k += _n; } while (0)
  for (const char *p = fmt; *p; p++) {
    if (*p != '%') { if (k + 1 >= cap) return 0; buf[k++] = *p; continue; }
    const char *start = p++;
    /* flags and width */
    char pad = 0; int upper = 0, swapcase = 0, width = -1;
    for (;; p++) {
      if (*p == '-') pad = '-';
      else if (*p == '_') pad = ' ';
      else if (*p == '0') pad = '0';
      else if (*p == '^') upper = 1;
      else if (*p == '#') swapcase = 1;
      else break;
    }
    if (isdigit((unsigned char)*p)) { width = 0; while (isdigit((unsigned char)*p)) width = width * 10 + (*p++ - '0'); }
    if (*p == 'E' || *p == 'O') p++;
    char v[128]; v[0] = 0;
    int num = 0; long nv = 0; int nw = 2; char npad = '0';
    switch (*p) {
      case 'a': snprintf(v, sizeof v, "%.3s", t->tm_wday >= 0 && t->tm_wday < 7 ? wd[t->tm_wday] : "?"); break;
      case 'A': snprintf(v, sizeof v, "%s", t->tm_wday >= 0 && t->tm_wday < 7 ? wd[t->tm_wday] : "?"); break;
      case 'b': case 'h': snprintf(v, sizeof v, "%.3s", t->tm_mon >= 0 && t->tm_mon < 12 ? mo[t->tm_mon] : "?"); break;
      case 'B': snprintf(v, sizeof v, "%s", t->tm_mon >= 0 && t->tm_mon < 12 ? mo[t->tm_mon] : "?"); break;
      case 'c': sp_w32_strftime(v, sizeof v, "%a %b %e %H:%M:%S %Y", t); break;
      case 'C': num = 1; nv = (t->tm_year + 1900) / 100; break;
      case 'd': num = 1; nv = t->tm_mday; break;
      case 'D': sp_w32_strftime(v, sizeof v, "%m/%d/%y", t); break;
      case 'e': num = 1; nv = t->tm_mday; npad = ' '; break;
      case 'F': sp_w32_strftime(v, sizeof v, "%Y-%m-%d", t); break;
      case 'g': { int y; sp_w32_iso_week(t, &y); num = 1; nv = ((y % 100) + 100) % 100; break; }
      case 'G': { int y; sp_w32_iso_week(t, &y); num = 1; nv = y; nw = 4; break; }
      case 'H': num = 1; nv = t->tm_hour; break;
      case 'I': num = 1; nv = t->tm_hour % 12 ? t->tm_hour % 12 : 12; break;
      case 'j': num = 1; nv = t->tm_yday + 1; nw = 3; break;
      case 'k': num = 1; nv = t->tm_hour; npad = ' '; break;
      case 'l': num = 1; nv = t->tm_hour % 12 ? t->tm_hour % 12 : 12; npad = ' '; break;
      case 'm': num = 1; nv = t->tm_mon + 1; break;
      case 'M': num = 1; nv = t->tm_min; break;
      case 'n': snprintf(v, sizeof v, "\n"); break;
      case 'p': snprintf(v, sizeof v, "%s", t->tm_hour < 12 ? "AM" : "PM"); if (swapcase) { v[0] = (char)tolower(v[0]); v[1] = (char)tolower(v[1]); swapcase = 0; } break;
      case 'P': snprintf(v, sizeof v, "%s", t->tm_hour < 12 ? "am" : "pm"); break;
      case 'r': sp_w32_strftime(v, sizeof v, "%I:%M:%S %p", t); break;
      case 'R': sp_w32_strftime(v, sizeof v, "%H:%M", t); break;
      case 's': { struct tm c = *t; snprintf(v, sizeof v, "%lld", (long long)mktime(&c)); break; }
      case 'S': num = 1; nv = t->tm_sec; break;
      case 't': snprintf(v, sizeof v, "\t"); break;
      case 'T': sp_w32_strftime(v, sizeof v, "%H:%M:%S", t); break;
      case 'u': num = 1; nv = t->tm_wday ? t->tm_wday : 7; nw = 1; break;
      case 'U': num = 1; nv = (t->tm_yday + 7 - t->tm_wday) / 7; break;
      case 'V': num = 1; nv = sp_w32_iso_week(t, NULL); break;
      case 'w': num = 1; nv = t->tm_wday; nw = 1; break;
      case 'W': num = 1; nv = (t->tm_yday + 7 - (t->tm_wday + 6) % 7) / 7; break;
      case 'x': sp_w32_strftime(v, sizeof v, "%m/%d/%y", t); break;
      case 'X': sp_w32_strftime(v, sizeof v, "%H:%M:%S", t); break;
      case 'y': num = 1; nv = ((t->tm_year + 1900) % 100 + 100) % 100; break;
      case 'Y': num = 1; nv = t->tm_year + 1900; nw = 1; break;
      case 'z': {
        long tzs = 0, dst = 0;
        _get_timezone(&tzs); _get_dstbias(&dst);
        long off = -(tzs + (t->tm_isdst > 0 ? dst : 0));
        long a = off < 0 ? -off : off;
        snprintf(v, sizeof v, "%c%02ld%02ld", off < 0 ? '-' : '+', a / 3600, (a / 60) % 60);
        break;
      }
      case 'Z': sp_w32_zone_abbr(t->tm_isdst, v, sizeof v); if (swapcase) { for (char *q = v; *q; q++) *q = (char)tolower((unsigned char)*q); swapcase = 0; } break;
      case '%': snprintf(v, sizeof v, "%%"); break;
      case 0: p--; /* fall through */
      default:
        /* not a directive: copied as written, as glibc does */
        snprintf(v, sizeof v, "%.*s", (int)(p - start + 1), start);
        width = -1;
        break;
    }
    if (num) {
      int w = width >= 0 ? width : nw;
      char pc = pad ? pad : npad;
      if (pc == '-') snprintf(v, sizeof v, "%ld", nv);
      else if (pc == ' ') snprintf(v, sizeof v, "%*ld", w, nv);
      else snprintf(v, sizeof v, nv < 0 ? "-%0*ld" : "%0*ld", nv < 0 ? w - 1 : w, nv < 0 ? -nv : nv);
    } else if (width > 0 && (int)strlen(v) < width) {
      char tmp[128]; int n = (int)strlen(v);
      char pc = pad == '0' ? '0' : ' ';
      memset(tmp, pc, (size_t)(width - n)); memcpy(tmp + width - n, v, (size_t)n + 1);
      snprintf(v, sizeof v, "%s", tmp);
    }
    if (upper || swapcase) for (char *q = v; *q; q++) *q = (char)toupper((unsigned char)*q);
    SP_W32_PUTS(v);
  }
  if (k >= cap) return 0;
  buf[k] = 0;
  return k;
#undef SP_W32_PUTS
}
#endif /* _WIN32 */
