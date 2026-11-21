# The Errno message of read_nonblock and write_nonblock: the bare strerror
# for a socket or a pipe, and "@ <function> - <path>" for a File.
require "socket"
require "tmpdir"

# CRuby ignores SIGPIPE from the start; a Spinel program does not in each
# case, and the pipe write below could end it
trap("PIPE", "IGNORE")

def attempt
  yield
  :ok
rescue SystemCallError => e
  "#{e.class}: #{e.message}"
end

un = Socket.new(Socket::AF_INET, Socket::SOCK_STREAM)
begin
  un.read_nonblock(10)
rescue SystemCallError => e
  # glibc's strerror reads "Transport endpoint is not connected"
  p e.class, e.message == Errno::ENOTCONN.new.message
end
un.close

r, w = IO.pipe
r.close
p attempt { w.write_nonblock("x") }
w.close

dir = Dir.mktmpdir
d = File.open(dir)
p attempt { d.read_nonblock(10) }.sub(dir, "DIR")
d.close
Dir.rmdir(dir)
