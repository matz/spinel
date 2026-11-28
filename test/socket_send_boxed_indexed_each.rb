# send on a socket that a boxed receiver's each_with_index (or
# each.with_index) hands to a map or select block -- an Array that is a
# thread's parameter, a local of two kinds -- is BasicSocket#send in each of
# its forms, as on an Array the program types: the element is boxed while the
# types settle, so the call is the boxed socket write and not Object#send
# with a name known only at run time.
require "socket"

rx = UDPSocket.new
rx.bind("127.0.0.1", 0)
port = rx.addr[1]
sa = Socket.sockaddr_in(port, "127.0.0.1")

def drain(rx, n) = n.times.map { rx.recvfrom(16)[0] }.sort

connected = [UDPSocket.new, UDPSocket.new]
connected.each { |s| s.connect("127.0.0.1", port) }
t = Thread.new(connected) do |socks|
  socks.each_with_index.map { |s, i| s.send("c#{i}", 0) }
end
p t.value
p drain(rx, 2)

loose = [UDPSocket.new, UDPSocket.new]
t = Thread.new(loose, sa) do |socks, dest|
  socks.each_with_index.map { |s, i| s.send("a#{i}", 0, dest) }
end
p t.value
p drain(rx, 2)

t = Thread.new(loose) do |socks|
  socks.each.with_index.map { |s, i| s.send("h#{i}", 0, "127.0.0.1", port) }
end
p t.value
p drain(rx, 2)

t = Thread.new(loose, sa) do |socks, dest|
  socks.each_with_index.select { |s, i| s.send("s#{i}", 0, dest) > 0 }.size
end
p t.value
p drain(rx, 2)

either = ARGV.size > 5 ? 1 : connected
p either.each_with_index.map { |s, i| s.send("e#{i}", 0) }
p drain(rx, 2)

# the same chain over values that are no sockets
p Thread.new([1, "a", :b]) { |xs| xs.each_with_index.map { |x, i| [x.class, i + 1] } }.value

(connected + loose).each(&:close)
rx.close
