require "socket"
srv = TCPServer.new("127.0.0.1", 0)
srv.setsockopt(Socket::SOL_SOCKET, Socket::SO_REUSEADDR, 1)
v = srv.getsockopt(Socket::SOL_SOCKET, Socket::SO_REUSEADDR)
p v.class
p v.data.bytes[0]
srv.close
