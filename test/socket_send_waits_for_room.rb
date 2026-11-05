# BasicSocket#send on a connected TCP socket waits for room in the send
# buffer, as #write does, rather than raising Errno::EAGAIN once the peer
# reads slower than the sender writes.
require "socket"

server = TCPServer.new("127.0.0.1", 0)
a = TCPSocket.new("127.0.0.1", server.addr[1])
b = server.accept
chunk = "x" * 1_000_000
reader = Thread.new do
  sleep 0.5
  total = 0
  total += b.readpartial(65536).bytesize while total < 30 * chunk.bytesize
  total
end
sent = 0
30.times do
  rest = chunk
  until rest.empty?
    n = a.send(rest, 0)
    sent += n
    rest = rest.byteslice(n..)
  end
end
p sent
p reader.value
a.close
b.close
server.close
