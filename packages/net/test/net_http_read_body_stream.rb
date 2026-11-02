# The peer will not finish its body until the consumer receives the first part.
# Reading the whole response before calling the block would deadlock.
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
received = Queue.new
t = Thread.new do
  c = server.accept
  while (line = c.gets)
    break if line.strip.empty?
  end
  c.write("HTTP/1.1 200 OK\r\nContent-Length: 32768\r\nConnection: close\r\n\r\n")
  c.write("a" * 16384)
  received.pop
  c.write("b" * 16384)
  c.close
end

total = 0
Net::HTTP.new("127.0.0.1", port).request(Net::HTTP::Get.new("/")) do |res|
  res.read_body do |chunk|
    received << true if total == 0 && !chunk.empty?
    total += chunk.bytesize
  end
end
p total
t.value
server.close
