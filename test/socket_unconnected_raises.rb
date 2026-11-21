# accept and recv on a socket that is neither connected nor listening raise
# at once as in CRuby: they waited for the socket to become readable, which
# on Darwin it never does.
require "socket"

def attempt
  yield
  :ok
rescue SystemCallError => e
  [e.class, e.message.sub(/\A.*?(?= - )/, "")]   # the strerror text differs across platforms
end

unconnected = Socket.new(Socket::AF_INET, Socket::SOCK_STREAM)
p attempt { unconnected.accept }
p attempt { unconnected.recv(10) }
p attempt { unconnected.recv(100_000) }
p attempt { unconnected.recvfrom(10) }

bound = Socket.new(Socket::AF_INET, Socket::SOCK_STREAM)
bound.bind(Socket.sockaddr_in(0, "127.0.0.1"))
p attempt { bound.accept }

# (Linux's EOPNOTSUPP is ENOTSUP, whose class inspects as Errno::ENOTSUP)
datagram = Socket.new(Socket::AF_INET, Socket::SOCK_DGRAM)
begin
  datagram.accept
rescue SystemCallError => e
  p [e.is_a?(Errno::EOPNOTSUPP), e.message.sub(/\A.*?(?= - )/, "")]
end

# a listening socket still accepts a pending connection and reads from it
server = TCPServer.new("127.0.0.1", 0)
client = TCPSocket.new("127.0.0.1", server.addr[1])
peer = server.accept
client.write("hi")
p peer.recv(2)

peer.close
client.close
server.close
unconnected.close
bound.close
datagram.close
