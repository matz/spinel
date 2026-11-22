# const_get reads the constants of a builtin class or module that a path
# such as Process::WNOHANG reads.
p Process.const_get(:WNOHANG) == Process::WNOHANG
p Process.const_get("CLOCK_MONOTONIC") == Process::CLOCK_MONOTONIC
p Process.const_get(:RLIMIT_NOFILE) == Process::RLIMIT_NOFILE
p Process.const_get(:Status)
p Math.const_get(:DomainError)
p Float.const_get(:MAX_EXP)
p File.const_get(:SEPARATOR)
p File.const_get(:SEEK_END)
p Regexp.const_get(:IGNORECASE)
p Encoding.const_get(:UTF_8)
p((Process.const_get(:NoSuchThing) rescue $!.message))
