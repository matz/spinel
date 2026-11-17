# A computed send that hands its block on (`&block`), beside a class's own
# send: the other classes reach Object#send with the block.
class Sock
  def send(data) = data.size
end
class Ev
  def transitions(*a, &b) = [a, b ? b.call : nil]
end
class Proxy
  def initialize(src) = @src = src
  def method_missing(name, *args, &block) = @src.send(name, *args, &block)
end
pr = Proxy.new([Ev.new, Sock.new][ARGV.size])
p pr.transitions(1, 2) { :blk }
p Sock.new.send("abc")
