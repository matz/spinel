# Socket#connect_nonblock on a socket that is already connected raises
# Errno::EISCONN, as CRuby's does; only exception: false answers 0 for it.
require "socket"

srv = TCPServer.new("127.0.0.1", 0)
addr = Socket.sockaddr_in(srv.addr[1], "127.0.0.1")

s = Socket.new(Socket::AF_INET, Socket::SOCK_STREAM, 0)
first = s.connect_nonblock(addr, exception: false)
puts(first == 0 || first == :wait_writable)
puts IO.select(nil, [s], nil, 5) ? "writable" : "timeout"
# Once the handshake is done, Linux answers this call 0 and macOS EISCONN;
# under exception: false both come back as 0.
p s.connect_nonblock(addr, exception: false)
# Past that, every kernel says EISCONN.
p s.connect_nonblock(addr, exception: false)
begin
  s.connect_nonblock(addr)
  puts "no raise"
rescue Errno::EISCONN => e
  puts e.class
end
begin
  s.connect_nonblock(addr, exception: true)
  puts "no raise"
rescue SystemCallError => e
  puts e.class
end
s.close
srv.close
