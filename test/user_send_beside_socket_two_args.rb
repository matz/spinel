# A computed send of two arguments, in a program that requires socket,
# on a value that may be a class with its own `send` or another: the
# other reaches Object#send, and a socket still takes BasicSocket#send.
require "socket"
class Sock
  def send(data) = data.size
end
class Configuration
  attr_accessor :column
end
c = Configuration.new
pick = [c, Sock.new][ARGV.size]
key = :column
pick.send("#{key}=", :status)
p c.column
p [Sock.new, c][0].send("abc")
a = UDPSocket.new
a.bind("127.0.0.1", 0)
b = UDPSocket.new
b.connect("127.0.0.1", a.addr[1])
p [b, c][ARGV.size].send("hi", 0)
p a.recvfrom(10)[0]
