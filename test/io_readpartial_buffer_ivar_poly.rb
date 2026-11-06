# readpartial(n, buf) on a receiver that is an IO or another class with the
# same method, into a buffer held in an instance variable that is also
# appended to: the IO fills the buffer in place, a frozen one raises before
# reading, and the end of the stream raises and empties it.
def try(label)
  r = yield
  puts "#{label}: #{r.inspect}"
rescue => e
  puts "#{label}: #{e.class}: #{e.message}"
end

class Stream
  def readpartial(n, buf)
    buf.replace("abc")
    buf
  end
end

class Reader
  def initialize(io, buf)
    @io = io
    @chunk = buf
  end

  def take(n)
    @io.readpartial(n, @chunk)
    @chunk << "!"
    @chunk
  end
end

try("a stand-in") { Reader.new(Stream.new, String.new(capacity: 16)).take(3) }

r, w = IO.pipe
w.write("xyz")
reader = Reader.new(r, String.new(capacity: 16))
try("an IO") { reader.take(3) }
w.write("é")
try("text keeps its encoding") { got = Reader.new(r, +"text").take(10); [got, got.encoding] }
w.close
try("the end of the stream") { reader.take(3) }

local = +"local"
local << ""
r2, w2 = IO.pipe
w2.write("hey")
try("a local buffer") { [r2.readpartial(10, local), local.encoding] }
w2.close
try("a local at the end") { r2.readpartial(10, local) }
try("the local after the end") { local }

r3, w3 = IO.pipe
w3.write("left")
try("a frozen buffer") { Reader.new(r3, "abc".freeze).take(3) }
try("the bytes stay unread") { r3.readpartial(10) }
