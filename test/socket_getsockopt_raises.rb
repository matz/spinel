# getsockopt raises the Errno of a failing getsockopt(2), as CRuby does: it
# returned a Socket::Option holding -1.
require "socket"

server = TCPServer.new("127.0.0.1", 0)
s = TCPSocket.new("127.0.0.1", server.addr[1])

begin
  s.getsockopt(Socket::SOL_SOCKET, 0x7fff)
  p :no_raise
rescue SystemCallError => e
  p e.class, e.message
end
p((s.getsockopt(Socket::IPPROTO_IPV6, Socket::IPV6_V6ONLY) rescue $!.message.end_with?(" - getsockopt(2)")))
p s.getsockopt(Socket::SOL_SOCKET, Socket::SO_ERROR).int
s.close
server.close
