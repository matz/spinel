@echo off
rem Spinel AOT Compiler - Ruby to native binary (Windows)
rem
rem Usage:
rem   spinel app.rb                  - compiles to .\app.exe
rem   spinel app.rb -o myapp         - compiles to .\myapp.exe
rem   spinel app.rb -c               - generate C only (app.c)
rem   spinel app.rb -S               - print C to stdout
rem
rem Options:
rem   -o FILE    Output file (binary, or .c when combined with -c)
rem   -c         Generate C source only, don't compile
rem   -S         Print generated C to stdout
rem   -O LEVEL   Optimization level for cc (default: 2)
rem   --cc=CMD   C compiler command (default: cc)
rem   --lonig    Link with oniguruma (for regexp programs)

setlocal

set "DIR=%~dp0"
if "%DIR:~-1%"=="\" set "DIR=%DIR:~0,-1%"

set "PARSE_RB=%DIR%\spinel_parse.rb"
set "PARSE_BIN=%DIR%\spinel_parse.exe"
set "CODEGEN_RB=%DIR%\spinel_codegen.rb"
set "CODEGEN_BIN=%DIR%\spinel_codegen.exe"

set "SOURCE="
set "OUTPUT="
set "C_ONLY=0"
set "STDOUT_MODE=0"
set "OPT_LEVEL=2"
set "CC_CMD=cc"
set "EXTRA_FLAGS="

:parse
if "%~1"=="" goto endparse
if /i "%~1"=="-o" (set "OUTPUT=%~2" & shift & shift & goto parse)
if /i "%~1"=="-O" (set "OPT_LEVEL=%~2" & shift & shift & goto parse)
if /i "%~1"=="-c" (set "C_ONLY=1" & shift & goto parse)
if /i "%~1"=="-S" (set "STDOUT_MODE=1" & shift & goto parse)
if /i "%~1"=="--lonig" (
  if defined EXTRA_FLAGS (
    set "EXTRA_FLAGS=%EXTRA_FLAGS% -lonig"
  ) else (
    set "EXTRA_FLAGS=-lonig"
  ) & shift & goto parse
)
set "_a=%~1"
if "%_a:~0,5%"=="--cc=" (set "CC_CMD=%_a:~5%" & shift & goto parse)
if not defined SOURCE set "SOURCE=%~1"
shift
goto parse
:endparse

if not defined SOURCE (
  echo Spinel AOT Compiler 1>&2
  echo. 1>&2
  echo Usage: spinel app.rb              - compile to .\app.exe 1>&2
  echo        spinel app.rb -o myapp     - compile to .\myapp.exe 1>&2
  echo        spinel app.rb -c           - generate app.c only 1>&2
  echo        spinel app.rb -S           - print C to stdout 1>&2
  exit /b 1
)

if not exist "%SOURCE%" (
  echo spinel: %SOURCE%: No such file 1>&2
  exit /b 1
)

for %%F in ("%SOURCE%") do set "BASENAME=%%~nF"

set "AST_TMP=%TEMP%\spinel_ast_%RANDOM%%RANDOM%.tmp"

rem ---- Step 1: parse ----
if exist "%PARSE_BIN%" (
  "%PARSE_BIN%" "%SOURCE%" "%AST_TMP%"
) else (
  ruby -E UTF-8:UTF-8 "%PARSE_RB%" "%SOURCE%" > "%AST_TMP%"
)
if errorlevel 1 (
  echo spinel: parse failed 1>&2
  if exist "%AST_TMP%" del "%AST_TMP%"
  exit /b 1
)

rem ---- Step 2: codegen ----
if "%STDOUT_MODE%"=="1" goto stdout_codegen

set "C_TMP="
if "%C_ONLY%"=="1" goto cfile_for_conly
set "C_TMP=%TEMP%\spinel_out_%RANDOM%%RANDOM%.c"
set "C_FILE=%C_TMP%"
goto do_codegen

:cfile_for_conly
if defined OUTPUT (
  set "C_FILE=%OUTPUT%"
) else (
  set "C_FILE=%BASENAME%.c"
)

:do_codegen
if exist "%CODEGEN_BIN%" (
  "%CODEGEN_BIN%" "%AST_TMP%" "%C_FILE%"
) else (
  ruby -E UTF-8:UTF-8 "%CODEGEN_RB%" "%AST_TMP%" "%C_FILE%"
)
if errorlevel 1 (
  echo spinel: codegen failed 1>&2
  if exist "%AST_TMP%" del "%AST_TMP%"
  if defined C_TMP if exist "%C_TMP%" del "%C_TMP%"
  exit /b 1
)
if exist "%AST_TMP%" del "%AST_TMP%"

if "%C_ONLY%"=="1" (
  echo Wrote %C_FILE% 1>&2
  exit /b 0
)

rem ---- Step 3: compile ----
if defined OUTPUT (
  set "BIN_FILE=%OUTPUT%"
) else (
  set "BIN_FILE=%BASENAME%"
)

set "SP_RT_LIB=%DIR%\lib\libspinel_rt.a"
set "INCLUDE_FLAGS=-I"%DIR%\lib" -I"%DIR%\lib\regexp""
if exist "%SP_RT_LIB%" (
  if defined EXTRA_FLAGS (
    set "EXTRA_FLAGS=%EXTRA_FLAGS% "%SP_RT_LIB%""
  ) else (
    set "EXTRA_FLAGS="%SP_RT_LIB%""
  )
)

%CC_CMD% -O%OPT_LEVEL% -Wno-all -ffunction-sections -fdata-sections %INCLUDE_FLAGS% "%C_FILE%" -lm %EXTRA_FLAGS% -Wl,--gc-sections -Wl,--stack,67108864 -o "%BIN_FILE%"
set "EC=%ERRORLEVEL%"
if defined C_TMP if exist "%C_TMP%" del "%C_TMP%"
if not "%EC%"=="0" (
  echo spinel: C compilation failed 1>&2
  exit /b 1
)

echo %SOURCE% -^> %BIN_FILE%.exe 1>&2
exit /b 0

:stdout_codegen
if exist "%CODEGEN_BIN%" (
  "%CODEGEN_BIN%" "%AST_TMP%"
) else (
  ruby -E UTF-8:UTF-8 "%CODEGEN_RB%" "%AST_TMP%"
)
set "EC=%ERRORLEVEL%"
if exist "%AST_TMP%" del "%AST_TMP%"
exit /b %EC%
