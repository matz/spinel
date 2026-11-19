# A bare warn while $stderr can hold a program object with its own write:
# the messages, rendered as puts renders them, go to that write in one call
require "stringio"
class Sink
  attr_reader :buf
  def initialize = @buf = []
  def write(s) = (@buf << s; s.size)
end
class Pt
  def to_s = "pt"
end
class Loud
  def write(s) = 0
  def go = warn("from Loud")
end
def with_stderr(err)
  old = $stderr
  $stderr = err
  yield
ensure
  $stderr = old
end
s = Sink.new
with_stderr(s) do
  x = warn("v", nil, Pt.new, ["line\n", [:sym]])
  p x
  Loud.new.go
  $VERBOSE = nil
  warn "silenced"
  $VERBOSE = false
  warn "back"
end
p s.buf
io = StringIO.new
with_stderr(io) { warn "into sio", "two" }
p io.string
binary = StringIO.new
message = [1, +"a\0b\n"].last
with_stderr(binary) { warn [message] }
p binary.string.bytes
with_stderr(Loud.new) { warn "dropped" }
with_stderr(s) { warn; warn [] }
p s.buf.last(2)
# the messages run before $stderr is read and before $VERBOSE is tested,
# and that test calls no nil? of the program's
class Q
  def nil? = true
end
def side(x)
  puts "side #{x}"
  x
end
s2 = Sink.new
s3 = Sink.new
with_stderr(s2) do
  $VERBOSE = nil
  warn side("a")
  $VERBOSE = Q.new
  warn side("b")
  $VERBOSE = false
  warn(($stderr = s3; "moved"))
end
p s2.buf, s3.buf
