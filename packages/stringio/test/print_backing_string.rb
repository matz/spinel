require "stringio"
class Echo
 def initialize(s) = (@s=s)
 def to_s = @s
end
s = +"abcd"
io = StringIO.new(s)
io.pos = 1
io.print Echo.new(s)
p io.string, s
s2 = +("abcd" * 64)
io2 = StringIO.new(s2)
io2.pos = 1
io2.print Echo.new(s2)
p io2.string == "a" + "abcd" * 64, io2.string.bytesize
