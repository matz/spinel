require "socket"
# Echo server: TCPServer accepts one connection, the client sends, server
# echoes, client receives. Exercises bind/listen/accept/connect/sendmsg/recv.
srv = TCPServer.new("127.0.0.1", 0)
port = srv.addr[1]
c = TCPSocket.new("127.0.0.1", port)
s = srv.accept
s.sendmsg("hello world")
buf = c.recv(1024)
p buf
# local/remote addresses report the right family + a numeric port.
p c.local_address.family
p c.local_address.ip_address
p c.remote_address.ip_address
s.close
c.close
srv.close
