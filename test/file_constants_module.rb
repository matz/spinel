# File::Constants::X and IO::X read File's open, flock and fnmatch
# constants, as File::Constants is a module File and IO include.
require "tmpdir"

p File::Constants::RDONLY
p File::Constants::BINARY
p File::Constants::LOCK_EX
p File::Constants::FNM_DOTMATCH
p File::Constants::NULL
p ::File::Constants::WRONLY
p File::Constants::CREAT == File::CREAT
p File::Constants::NONBLOCK == File::NONBLOCK
p IO::RDWR
p IO::LOCK_NB
p IO::FNM_PATHNAME
p IO::TRUNC == File::TRUNC
p IO::SEEK_END

path = File.join(Dir.tmpdir, "spinel_file_constants_#{Process.pid}")
File.open(path, File::Constants::WRONLY | File::Constants::CREAT | File::Constants::TRUNC) do |f|
  f.flock(IO::LOCK_EX)
  f.write("abc")
end
p File.read(path)
File.delete(path)
p File.fnmatch("*", ".x", File::Constants::FNM_DOTMATCH)
p ::IO::RDWR
p ::IO::LOCK_UN
p ::File::Constants::APPEND == ::File::APPEND
