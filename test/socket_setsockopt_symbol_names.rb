# setsockopt and getsockopt take the level and option as a Symbol or String,
# with or without the prefix (:TCP and :IPPROTO_TCP, :NODELAY and
# :TCP_NODELAY), as CRuby does: a Symbol raised TypeError.
require "socket"

server = TCPServer.new("127.0.0.1", 0)
s = TCPSocket.new("127.0.0.1", server.addr[1])

p s.setsockopt(:TCP, :NODELAY, true)
p s.getsockopt(:TCP, :NODELAY).bool
s.setsockopt("IPPROTO_TCP", "TCP_NODELAY", false)
p s.getsockopt(Socket::IPPROTO_TCP, Socket::TCP_NODELAY).bool
s.setsockopt(:SOCKET, :KEEPALIVE, 1)
p s.getsockopt(:SOL_SOCKET, :SO_KEEPALIVE).bool
s.setsockopt(Socket::SOL_SOCKET, :KEEPALIVE, 0)
p s.getsockopt("SOCKET", Socket::SO_KEEPALIVE).bool
s.setsockopt(:TCP, Socket::TCP_NODELAY, 1)
p s.getsockopt(:TCP, "NODELAY").bool

# IP-level names, and the Socket::IP_TOS constant
s.setsockopt(:IP, :TOS, 16)
p s.getsockopt(:IPPROTO_IP, :IP_TOS).int, Socket::IP_TOS == s.getsockopt(:IP, :TOS).optname

# a name held in a mixed Array is boxed
level, opt = [:TCP, 0], [:NODELAY, 0]
s.setsockopt(level[0], opt[0], 0)
p s.getsockopt(level[0], opt[0]).bool

# a socket read back out of an Array is boxed
boxed = [s, 0][0]
p boxed.setsockopt(:SOCKET, :KEEPALIVE, true)
p boxed.getsockopt(:SOCKET, :KEEPALIVE).bool

# beside a class with its own setsockopt, the socket's call takes names too
class OptRecorder
  def setsockopt(level, opt, value) = [:recorded, level, opt, value]
end
[s, OptRecorder.new].each { |x| p x.setsockopt(:TCP, :NODELAY, true) }
[s, OptRecorder.new].each { |x| p x.setsockopt("SOCKET", "KEEPALIVE", false) }
p s.getsockopt(:TCP, :NODELAY).bool, s.getsockopt(:SOCKET, :KEEPALIVE).bool

# a name built in a mutable String reaches that call as a handle
level, opt = +"TC", +"NODE"
level << "P"
opt << "LAY"
[s, OptRecorder.new].each { |x| p x.setsockopt(level, opt, false) }
p s.getsockopt(level, opt).bool

# the option's packed bytes as the value reach that call too
[s, OptRecorder.new].each { |x| p x.setsockopt(:TCP, :NODELAY, [1].pack("i")) }
p s.getsockopt(:TCP, :NODELAY).bool
[s, OptRecorder.new].each { |x| p x.setsockopt(:TCP, :NODELAY, [0].pack("i")) }
p s.getsockopt(:TCP, :NODELAY).bool

def attempt
  yield
rescue => e
  [e.class, e.message]
end
p attempt { s.setsockopt(:FOO, :NODELAY, 1) }
p attempt { s.setsockopt(:tcp, :nodelay, 1) }
p attempt { s.setsockopt(:TCP, :FOO, 1) }
p attempt { s.setsockopt(:SOCKET, :NODELAY, 1) }
p attempt { s.getsockopt(:UDP, :NODELAY) }
p attempt { s.setsockopt(:ICMPV6, :FOO, 1) }
p attempt { s.setsockopt("TCP\0junk", "NODELAY", 1) }
p attempt { s.setsockopt("TCP", "NODELAY\0junk", 1) }
p attempt { s.setsockopt(:"SOCKET\0junk", :KEEPALIVE, 1) }
p attempt { s.getsockopt(:TCP, :"NODELAY\0junk") }
p attempt { s.setsockopt(:TCP, true, 1) }
p attempt { s.setsockopt(:TCP, nil, 1) }
[s, OptRecorder.new].each { |x| p attempt { x.setsockopt(:TCP, true, 1) } }

s.close
server.close
