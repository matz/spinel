# A Socket read back out of an Array (a boxed handle) takes connect_nonblock,
# getsockopt and setsockopt as a typed one does: they raised NoMethodError.
require "socket"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
sockets = [Socket.new(Socket::AF_INET, Socket::SOCK_STREAM), Socket.new(Socket::AF_INET, Socket::SOCK_STREAM)]
sockets.each do |s|
  r = s.connect_nonblock(Socket.sockaddr_in(port, "127.0.0.1"), exception: false)
  p [:wait_writable, 0].include?(r)
end
_, writable, = IO.select(nil, sockets, nil, 5)
p writable.size >= 1   # how many of the two are ready at once is the kernel's (macOS answers one)
conn = sockets.fetch(sockets.index(writable.first))
p conn.getsockopt(Socket::SOL_SOCKET, Socket::SO_ERROR).int
begin
  conn.connect_nonblock(Socket.sockaddr_in(port, "127.0.0.1"))
  p :connected
rescue Errno::EISCONN   # Linux answers this first call after the handshake 0, macOS EISCONN
  p :connected
end
p conn.setsockopt(Socket::IPPROTO_TCP, Socket::TCP_NODELAY, 1)
p conn.getsockopt(Socket::IPPROTO_TCP, Socket::TCP_NODELAY).bool
sockets.each(&:close)
server.close
