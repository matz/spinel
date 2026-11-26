# An assignment to IO::X or File::X defines that class's own constant, and
# the other File::Constants reads keep CRuby's values.
IO::RDONLY = 7
p IO::RDONLY
p File::Constants::RDONLY
p IO::WRONLY
IO::APPEND, other = 11, nil
p IO::APPEND
p File::Constants::CREAT == File::CREAT
File::LOCK_EX = 9
p File::LOCK_EX
p IO::LOCK_SH
