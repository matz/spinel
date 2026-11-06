# Every kind of read on one TCP socket takes the same buffered bytes in order:
# what #gets leaves is what #getc, #ungetc, #read, #readpartial and
# #read_nonblock see next, and IO.select reports it without waiting.
require "socket"
server = TCPServer.new("127.0.0.1", 0)
client = TCPSocket.new("127.0.0.1", server.addr[1])
s = server.accept
client.write("line one\nline two\nabcdefghij")
p s.gets
ready = IO.select([s], nil, nil, 0)
p ready && ready[0].size
p s.getc
s.ungetc("L")
p s.gets
p s.read(3)
p s.readpartial(4)
p s.read(2)
p s.read_nonblock(10)
begin
  s.read_nonblock(10)
rescue IO::WaitReadable => e
  p e.is_a?(IO::WaitReadable)
end
client.close
p s.read
p s.eof?
s.close
server.close
