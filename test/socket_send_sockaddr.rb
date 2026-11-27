# UDPSocket#send(mesg, flags, dest_sockaddr), BasicSocket's three-argument
# form: the destination is a packed sockaddr, as Socket.sockaddr_in,
# Socket.pack_sockaddr_in and Addrinfo#to_sockaddr make it, given to
# sendto(2) as it is, so no name is resolved on each datagram. It answers
# the byte count; the receiver sees the payload as BINARY. A sockaddr too
# short for its family is an error from sendto(2) itself (EINVAL on macOS,
# EAFNOSUPPORT on Linux, as under CRuby, so only that much is printed); nil
# is no destination, the connected peer's send(2), whose error for a socket
# with no peer is EDESTADDRREQ on macOS and Linux and ENOTCONN on Windows. A Socket's send takes the same form, and a
# closed socket raises IOError.
require "socket"

rx = UDPSocket.new
rx.bind("127.0.0.1", 0)
port = rx.addr[1]
tx = UDPSocket.new

sa = Socket.sockaddr_in(port, "127.0.0.1")
p tx.send("a", 0, sa)
p rx.recvfrom(16)[0]

p tx.send("bc", 0, Addrinfo.udp("127.0.0.1", port).to_sockaddr)
p rx.recvfrom(16)[0]

p tx.send("é", 0, Socket.pack_sockaddr_in(port, "127.0.0.1"))
m = rx.recvfrom(16)[0]
p [m, m.encoding]

# A payload and an address held in variables, sent many times over: the
# address is built once and each send is one sendto(2).
payload = "x" * 1200
sent = 0
10.times { sent += tx.send(payload, 0, sa) }
got = 0
10.times { got += rx.recv(2048).bytesize }
p [sent, got]

begin
  tx.send("z", 0, "bad")
rescue SystemCallError => e
  p [e.class.name.start_with?("Errno::"), e.message.end_with?(" - sendto(2)")]
end

begin
  tx.send("z", 0, nil)
rescue SystemCallError => e
  p [e.class.name.start_with?("Errno::"), e.message.end_with?(" - send(2)")]
end

s = Socket.new(:INET, :DGRAM)
p s.send("sock", 0, sa)
p rx.recvfrom(16)[0]
s.close

tx.close
begin
  tx.send("z", 0, sa)
rescue IOError => e
  puts "#{e.class}: #{e.message}"
end
rx.close
