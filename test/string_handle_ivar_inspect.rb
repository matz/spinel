# An ivar that holds a String's shared handle -- a reader hands it out and
# the caller appends through it -- is a String to the object's default
# inspect and to Marshal, as in CRuby. The inspect printed it as #<?>, and
# Marshal.dump raised TypeError (no marshal_dump) for the whole object.

class W
  attr_reader :buf
  def initialize = (@buf = +"ab"; @n = 1)
end
w = W.new
w.buf << "c"
puts w.inspect.sub(/0x\h+/, "0x")
p w.instance_variable_get(:@buf)

w2 = Marshal.load(Marshal.dump(w))
p w2.buf
w2.buf << "d"
p [w.buf, w2.buf]

# unset until a method sets it: nil, then the String
class V
  attr_reader :s
  def initialize = @s = nil
  def set = (@s = +"x"; self)
end
v = V.new
puts v.inspect.sub(/0x\h+/, "0x")
v.set.s << "y"
puts v.inspect.sub(/0x\h+/, "0x")
p Marshal.load(Marshal.dump(v)).s
p Marshal.load(Marshal.dump(V.new)).s
