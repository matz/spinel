# An Errno from a socket call names the system call as CRuby does
# ("Invalid argument - setsockopt(2)"): the message read
# "Invalid argument @ setsockopt - ".
require "socket"

def attempt
  yield
  :ok
rescue SystemCallError => e
  e.message.sub(/\A.*?(?= - | @ )/, "")   # the strerror text differs across platforms
end

server = TCPServer.new("127.0.0.1", 0)
s = TCPSocket.new("127.0.0.1", server.addr[1])
p attempt { s.setsockopt(Socket::IPPROTO_IPV6, Socket::IPV6_V6ONLY, 1) }
unconnected = Socket.new(Socket::AF_INET, Socket::SOCK_STREAM)
p attempt { unconnected.shutdown }
udp = UDPSocket.new
p attempt { Socket.for_fd(udp.fileno).listen(1) }
p attempt { udp.send("x", 0) }
p attempt { unconnected.accept_nonblock }
p attempt { Socket.new(99999, Socket::SOCK_STREAM) }
p attempt { Socket.socketpair(Socket::AF_INET, Socket::SOCK_STREAM) }

# a socket closed with bytes unread resets its peer: recv fails, from the
# buffer fill, from a read past the buffer, and without blocking
def reset_client
  server = TCPServer.new("127.0.0.1", 0)
  c = TCPSocket.new("127.0.0.1", server.addr[1])
  a = server.accept
  server.close
  c.write("unread")
  IO.select([a])
  a.close
  IO.select([c])
  c
end

c = reset_client
p attempt { c.recv(10) }
c.close
c = reset_client
p attempt { c.recv(100_000) }
c.close
c = reset_client
p attempt { c.recv_nonblock(10) }
c.close

# recv_nonblock and accept_nonblock raise IO::EAGAINWaitReadable when they would block
peer = server.accept
p attempt { s.recv_nonblock(10) }
p attempt { server.accept_nonblock }
peer.close
s.close
server.close
unconnected.close
udp.close
