require "socket"
# UDP datagram round-trip: bind a server, connect a client, send, recvfrom.
# recvfrom must return [mesg, Addrinfo]. We use the server's own bound port
# (read from us.addr[1]) rather than the sender's random ephemeral port so the
# output is deterministic.
us = UDPSocket.new(Socket::AF_INET)
us.bind(Socket.sockaddr_in(0, "127.0.0.1"))
uport = us.addr[1]
uc = UDPSocket.new(Socket::AF_INET)
uc.connect(Socket.sockaddr_in(uport, "127.0.0.1"))
uc.sendmsg("udp hi")
msg, from = us.recvfrom(1024)
p msg
p from.ip_address
p from.family
p (from.ip_address == "127.0.0.1")
us.close
uc.close
