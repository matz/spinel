# IPSocket.getaddress, on IPSocket and the classes below it, diffed
# against CRuby: the first address the name resolves to, and a SocketError
# for a name that resolves to none (CRuby 3.3+ raises its subclass
# Socket::ResolutionError, which SocketError rescues). It compiled to
# NoMethodError. Numeric hosts only, so nothing is resolved over the network.
require "socket"
p IPSocket.getaddress("127.0.0.1")
p IPSocket.getaddress("::1")
p TCPSocket.getaddress("10.1.2.3")
p UDPSocket.getaddress("192.168.0.1")
begin
  IPSocket.getaddress("no-such-host.invalid")
rescue SocketError
  p :rescued
end
