# spinel: gc-stress
# IO#read(n) larger than the socket's buffer, with the bytes arriving in
# pieces while another thread allocates, returns all n bytes in order.
require "socket"
server = TCPServer.new("127.0.0.1", 0)
left = TCPSocket.new("127.0.0.1", server.addr[1])
right = server.accept
piece = 256 * 1024
writer = Thread.new do
  12.times do |i|
    left.write((97 + i).chr * piece)
    sleep 0.01
  end
  left.close
  :written
end
allocator = Thread.new do
  junk = []
  20_000.times { |i| junk << "garbage #{i}" * 4; junk.clear if junk.size > 1000 }
  :allocated
end
data = right.read(3 * 1024 * 1024)
p data.bytesize
p (0...12).map { |i| data[i * piece] }.join
p data[piece - 1, 2]
p right.read(1)
p writer.value
p allocator.value
