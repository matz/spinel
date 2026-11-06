# readpartial(n, buf) fills buf in place and returns it: buf keeps its
# encoding, holds only the bytes read, is emptied at the end of the
# stream, and a read longer than what is waiting answers what is there.
require "socket"

def try(label)
  r = yield
  puts "#{label}: #{r.inspect}"
rescue => e
  puts "#{label}: #{e.class}: #{e.message}"
end

class Reader
  attr_reader :chunk

  def initialize(io)
    @io = io
    @chunk = String.new(capacity: 8)
  end

  def fill(n) = @io.readpartial(n, @chunk)
end

server = TCPServer.new("127.0.0.1", 0)
c = TCPSocket.new("127.0.0.1", server.addr[1])
s = server.accept
r = Reader.new(s)

c.write("hello")
try("fills the buffer") { [r.fill(16384), r.chunk.bytesize, r.chunk.encoding] }
try("returns the buffer") { c.write("x"); r.fill(10).equal?(r.chunk) }
c.write("a" * 20)
try("at most n") { r.fill(4) }
try("then the rest") { r.fill(100) }
c.write("b" * 40000)
total = 0
total += r.fill(65536).bytesize while total < 40000
try("a long read gets everything") { total }
try("zero empties it") { r.fill(0) }

text = +"héllo"
c.write("abc")
try("keeps its encoding") { [s.readpartial(10, text), text.encoding] }
frozen = "abc".freeze
try("frozen buffer") { s.readpartial(10, frozen) }

c.close
try("end of stream") { r.fill(10) }
try("buffer after the end") { r.chunk }

rd, wr = IO.pipe
p_reader = Reader.new(rd)
wr.write("pipe bytes")
try("a pipe") { p_reader.fill(100) }
wr.close
try("pipe end") { p_reader.fill(100) }
try("pipe buffer after the end") { p_reader.chunk }
s.close
server.close
