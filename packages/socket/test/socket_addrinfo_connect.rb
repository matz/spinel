require "socket"
# High-level Addrinfo#connect builds a Socket and connects; send/recv over it.
ai = Addrinfo.tcp("127.0.0.1", 0)
srv = TCPServer.new("127.0.0.1", 0)
port = srv.addr[1]
sock = Addrinfo.tcp("127.0.0.1", port).connect
s = srv.accept
s.sendmsg("via addrinfo")
buf = sock.recv(1024)
p buf
sock.close
s.close
srv.close
