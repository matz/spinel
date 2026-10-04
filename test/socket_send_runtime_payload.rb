# A socket's send(data, flags) is BasicSocket#send whatever holds the payload.
# A payload held in a variable was lowered as Object#send with a runtime
# method name, a dispatch over the program's method names on the payload's
# bytes, so `socket.send(packet, 0)` raised NoMethodError naming the packet's
# text (#7193). The literal form already had the #2922 guard; this covers the
# variable forms on a TCP socket and a connected UDP socket, the 4-argument
# UDP form with a variable host and port, and an Object#send with a runtime
# name beside them, which must keep lowering as before.
require "socket"

server = TCPServer.new("127.0.0.1", 0)
reader = Thread.new do
  peer = server.accept
  data = peer.read(11)
  peer.close
  data
end
client = TCPSocket.new("127.0.0.1", server.addr[1])
payload = "hello"
p client.send(payload, 0)
suffix = " world"
p client.send(suffix, 0)
client.close
p reader.value
server.close

rx = UDPSocket.new
rx.bind("127.0.0.1", 0)
host = "127.0.0.1"
port = rx.addr[1]
tx = UDPSocket.new
ping = "ping"
p tx.send(ping, 0, host, port)
p rx.recv(10)
tx.connect(host, port)
pong = "pong"
p tx.send(pong, 0)
p rx.recv(10)
tx.close
rx.close

class Greeter
  def hi = "hi"
  def bye = "bye"
end
name = ARGV.empty? ? :hi : :bye
p Greeter.new.send(name)
