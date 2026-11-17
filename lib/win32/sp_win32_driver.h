/* Windows: what the compiler driver (src/main.c) says differently when it
   builds a program here -- gcc, the .exe, the shim's headers and libraries
   on every program's command line ("@LIB@" is the runtime's lib directory),
   and two things done once at startup (sp_win32_driver.c). */
#ifndef SP_WIN32_DRIVER_H
#define SP_WIN32_DRIVER_H
#define SPINEL_DEFAULT_CC "gcc"
#define SPINEL_EXE_SUFFIX ".exe"
#define SPINEL_PLATFORM_CFLAGS "-I@LIB@/win32 -D_FILE_OFFSET_BITS=64 -DWIN32_LEAN_AND_MEAN"
#define SPINEL_PLATFORM_LIBS   "-L@LIB@/win32 -static -lws2_32 -lbcrypt -Wl,--stack,8388608"
/* $TMPDIR as the C compiler can use it, and gcc on PATH when RubyInstaller's
   Devkit or MSYS2 has one that is not */
void sp_w32_driver_init(void);
/* a path named on the command line, with slashes: the driver splits on '/',
   and the C it writes names the file in #line, where a backslash escapes */
const char *sp_w32_driver_path(const char *p);
/* a builtins/ or packages/ file the compiler read, with what a Windows
   build answers differently appended: lib/win32/overlay/<the same path>,
   reopening what the file defines (RbConfig's mingw32, Gem.win_platform?,
   Pathname's C:/ root). The file's own lines keep their numbers. */
char *sp_w32_overlay(char *content, const char *path);
/* a pre-installed package that is not there on Windows (pty): a require
   finds none, as for any library Spinel does not have */
int sp_w32_package_absent(const char *pkg);
#define SPINEL_PLATFORM_INIT()  sp_w32_driver_init()
#define SPINEL_PLATFORM_PATH(p) sp_w32_driver_path(p)
#endif
