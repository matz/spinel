# Every read on a handle that does not read raises "not opened for
# reading", as gets already did: a file opened for appending, and a socket
# after close_read. read_nonblock and recv still hand over what arrived
# before the read side was shut, as CRuby does, and handles that read still do.
require "socket"
require "tmpdir"

def try(label)
  r = yield
  puts "#{label}: #{r.inspect}"
rescue => e
  puts "#{label}: #{e.class}: #{e.message}"
end

path = File.join(Dir.tmpdir, "sp_io_read_checks_readable_#{Process.pid}")
File.write(path, "abc\ndef\n")
f = File.open(path, "a")
try("read") { f.read }
try("read(2)") { f.read(2) }
try("readpartial") { f.readpartial(2) }
try("sysread") { f.sysread(2) }
try("read_nonblock") { f.read_nonblock(2) }
try("getc") { f.getc }
try("readchar") { f.readchar }
try("getbyte") { f.getbyte }
try("readbyte") { f.readbyte }
try("each_char") { f.each_char { |c| c } }
try("ungetc") { f.ungetc("x") }
try("ungetbyte") { f.ungetbyte(65) }
try("eof?") { f.eof? }
try("gets, as before") { f.gets }
f.close

File.open(path, "r+") { |g| try("r+ reads") { [g.read(3), g.getc, g.eof?] } }
File.open(path, "w+") do |g|
  g.write("xyz")
  g.rewind
  try("w+ reads") { [g.getbyte, g.read] }
end
File.delete(path)

server = TCPServer.new("127.0.0.1", 0)
c = TCPSocket.new("127.0.0.1", server.addr[1])
s = server.accept
c.write("peer bytes\n")
c2 = TCPSocket.new("127.0.0.1", server.addr[1])
s2 = server.accept
c2.write("more")
try("an open socket reads") { s2.readpartial(4) }
s2.close
c2.close
s.close_read
try("socket read") { s.read }
try("socket readpartial") { s.readpartial(4) }
try("socket read_nonblock") { s.read_nonblock(4) }
try("socket getc") { s.getc }
try("socket recv") { s.recv(4) }
s.close
c.close
server.close
