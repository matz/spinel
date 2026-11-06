# spinel: gc-stress
# IO#read(n) on a TCP socket that holds fewer than n bytes parks until the
# rest arrives, so a collection another thread starts meanwhile does not wait
# on the reader's worker (the process froze at 0% CPU).
require "socket"
server = TCPServer.new("127.0.0.1", 0)
left = TCPSocket.new("127.0.0.1", server.addr[1])
right = server.accept
reader = Thread.new { right.read(3000).bytesize }
left.write("a" * 1000)
allocator = Thread.new do
  junk = []
  20_000.times { |i| junk << "garbage #{i}" * 4; junk.clear if junk.size > 1000 }
  :allocated
end
sleep 0.2
left.write("b" * 2000)
p reader.value
p allocator.value
