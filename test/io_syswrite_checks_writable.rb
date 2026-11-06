# syswrite on a handle that does not write raises IOError, as write does:
# a file opened for reading and a socket after close_write.
require "socket"
require "tmpdir"

def try(label)
  r = yield
  puts "#{label}: #{r.inspect}"
rescue => e
  puts "#{label}: #{e.class}: #{e.message}"
end

path = File.join(Dir.tmpdir, "sp_io_syswrite_checks_writable_#{Process.pid}")
File.write(path, "abc")
f = File.open(path, "r")
try("syswrite on a read-only file") { f.syswrite("z") }
try("the file is unchanged") { File.read(path) }
f.close
File.delete(path)

server = TCPServer.new("127.0.0.1", 0)
c = TCPSocket.new("127.0.0.1", server.addr[1])
s = server.accept
s.close_write
try("syswrite after close_write") { s.syswrite("z") }
try("the peer reads to the end") { c.read }
s.close
c.close
server.close
