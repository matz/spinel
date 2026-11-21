# frozen_string_literal: true
# spinel: gc-stress
# spinel: gc-minor
# spinel: int64
class ByteSubclass < String
  def inspect = "custom"
  def to_str
    raise "called subclass to_str"
  end
end

class PretendBytes
  def to_str
    raise "called pretend to_str"
  end
  def to_int
    raise "called pretend to_int"
  end
end

def effect(s, byte)
  s << "x"
  byte
end

# Every argument sees the mutation by effect before any bytes are appended.
s = +"a"
r = s.append_as_bytes(s, effect(s, 127), s)
p [s.bytes, r.equal?(s), s.encoding.name]

# The invalid argument suppresses the append, even after the last argument ran.
t = +"b"
begin
  t.append_as_bytes(65, PretendBytes.new, effect(t, 32))
rescue => e
  p [e.class, e.message, t]
end

s = +"é"
s.append_as_bytes(ByteSubclass.new("\xff".b), 2**96+7, -(2**96+7))
p [s.bytes, s.encoding.name]
begin
  "frozen".append_as_bytes(:bad)
rescue => e
  p [e.class, e.message]
end
begin
  "frozen".append_as_bytes
rescue => e
  p [e.class, e.message]
end
a = [0, 256, -1]
s = +"s"
s.append_as_bytes("!", *a, s)
p s.bytes

sub = ByteSubclass.new("ice").freeze
begin
  sub.append_as_bytes(97)
rescue FrozenError => e
  p [e.message, e.receiver.equal?(sub)]
end
