# spinel: gc-stress
# A large write to a TCP peer that reads slowly parks each time the send
# buffer fills and finishes, while another thread allocates.
require "socket"
server = TCPServer.new("127.0.0.1", 0)
client = TCPSocket.new("127.0.0.1", server.addr[1])
s = server.accept
chunk = "x" * (256 * 1024)
reader = Thread.new do
  got = 0
  while (part = s.readpartial(16 * 1024) rescue nil)
    got += part.bytesize
    sleep 0.001 if got % (256 * 1024) == 0
  end
  got
end
allocator = Thread.new do
  junk = []
  20_000.times { |i| junk << "garbage #{i}" * 4; junk.clear if junk.size > 1000 }
  :allocated
end
8.times { client.write(chunk) }
client.close
p reader.value
p allocator.value
