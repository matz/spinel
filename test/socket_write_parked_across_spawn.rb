# A write parked on a full socket buffer when a spawn hands the socket to a
# child, which turns its open file blocking, finishes once the peer reads:
# the write asks for the flag again after its park instead of sending as if
# the descriptor still would not block. On one worker, a send that slept in
# the kernel would keep the reader below from ever running.
ENV["SPINEL_WORKERS"] = "1"
require "socket"

server = TCPServer.new("127.0.0.1", 0)
left = TCPSocket.new("127.0.0.1", server.addr[1])
right = server.accept
size = 16_000_000
writer = Thread.new { left.write("x" * size) }
sleep 0.2                            # the write is parked on a full buffer
pid = spawn("sleep 0.1", in: left)   # the child takes the socket blocking
Process.wait(pid)
reader = Thread.new do
  total = 0
  total += right.readpartial(65536).bytesize while total < size
  total
end
p reader.value
p writer.value
left.close
right.close
server.close
