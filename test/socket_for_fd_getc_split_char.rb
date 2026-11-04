# spinel: gc-stress
# IO#getc on an IO.for_fd of a TCP socket returns a UTF-8 character whose
# bytes arrive apart while another thread allocates.
require "socket"
server = TCPServer.new("127.0.0.1", 0)
left = TCPSocket.new("127.0.0.1", server.addr[1])
right = server.accept
io = IO.for_fd(right.fileno, autoclose: false)
alloc = Thread.new do
  a = []
  100_000.times { |i| a << "g#{i}" * 8; a.clear if a.size > 500 }
  :allocated
end
chars = []
10.times do
  w = Thread.new { left.write("\xC3".b); sleep 0.02; left.write("\xA9".b) }
  chars << io.getc.bytes
  w.join
end
p chars.uniq
p alloc.value
