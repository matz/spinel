# An IO#dup and an IO.for_fd of a TCP socket wait for the rest of a line the
# peer sends in two parts, and a large write through them reaches a peer that
# reads slowly: neither reads the wait as the end of the stream or drops what
# does not fit at once.
require "socket"
server = TCPServer.new("127.0.0.1", 0)
client = TCPSocket.new("127.0.0.1", server.addr[1])
s = server.accept
d = s.dup
io = IO.for_fd(s.fileno, autoclose: false)
writer = Thread.new { client.write("on"); sleep 0.2; client.write("e\n") }
p d.gets
writer.join
writer = Thread.new { client.write("tw"); sleep 0.2; client.write("o\n") }
p io.gets
writer.join
big = "x" * (1024 * 1024)
reader = Thread.new do
  got = 0
  while got < 4 * big.bytesize
    got += client.readpartial(64 * 1024).bytesize
    sleep 0.001
  end
  got
end
4.times { io.print(big) }
io.flush
p reader.value
d.write("from dup\n")
p client.gets
io.print("p")
io.putc("c")
io.puts("!")
io.flush
p client.gets
io.close
d.close
s.close
client.close
server.close
