# Two threads reading one socket line by line, each on its own OS worker,
# take every byte the peer sent exactly once between them. A line can be
# split between the two, as in CRuby, so only the bytes are counted.
require "socket"
server = TCPServer.new("127.0.0.1", 0)
left = TCPSocket.new("127.0.0.1", server.addr[1])
right = server.accept
sent = 0
readers = 2.times.map do
  Thread.new do
    bytes = 0
    while (l = right.gets)
      bytes += l.bytesize
    end
    bytes
  end
end
2000.times do |i|
  line = "line #{i} " + "x" * (i % 300) + "\n"
  sent += line.bytesize
  left.write(line)
end
left.close
got = readers.map(&:value)
puts "bytes: #{got.sum == sent}"
