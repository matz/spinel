# IO#syswrite on an IO.for_fd of a TCP socket, to a reader slower than the
# writer, writes instead of raising Errno::EAGAIN.
require "socket"
server = TCPServer.new("127.0.0.1", 0)
left = TCPSocket.new("127.0.0.1", server.addr[1])
right = server.accept
io = IO.for_fd(right.fileno, autoclose: false)
data = "x" * 4_000_000
reader = Thread.new do
  got = 0
  while (c = (left.readpartial(65536) rescue nil))
    got += c.bytesize
    sleep 0.001
  end
  got
end
written = 0
written += io.syswrite(data.byteslice(written..)) while written < data.bytesize
right.shutdown(Socket::SHUT_WR)
puts "written: #{written}"
puts "read: #{reader.value}"
