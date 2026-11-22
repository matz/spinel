# A method call after IO#tap, on STDOUT, a File and a TCPServer.
require "socket"
require "tmpdir"

p STDOUT.tap { |s| s }.fileno

dir = Dir.mktmpdir
path = File.join(dir, "t.txt")
File.write(path, "abc")
f = File.open(path)
p f.tap { |h| h.read(1) }.read
f.close
p File.open(path).tap { |h| $last = h }.path == path
$last.close

port = TCPServer.new("127.0.0.1", 0).tap { |s| $srv = s }.addr[1]
p port == $srv.addr[1]
$srv.close

File.delete(path)
Dir.rmdir(dir)
