# spinel: gc-stress
# pack(fmt, buffer: b) puts the packed bytes on the end of b. It packed
# behind a copy of b's bytes and wrote the whole String back, so a message
# built by repeated packs into one buffer cost the square of its length.
# The bytes now go on the end; a format with @ or X, which can reach the
# bytes already in the buffer, takes the full path as before.

def show(tag, s) = p([tag, s, s.encoding, s.bytesize])

b = +"ab"                      # a UTF-8 buffer
r = [1, 2].pack("C*", buffer: b)
show(:c_star, b); p r.equal?(b)
u = +"x"
[0x263a].pack("U", buffer: u)
show(:u_fmt, u)
bb = "".b
[1.5].pack("E", buffer: bb); [7].pack("n", buffer: bb); ["hi"].pack("a3", buffer: bb)
show(:mixed, bb)
w = "".b
[300, 5].pack("w*", buffer: w)
show(:ber, w)
x = +"head"
[1].pack("xC", buffer: x)
show(:pad, x)

# cursor directives reach into the buffer's own bytes
at = +"abcdef"
[0x41].pack("@2C", buffer: at)
show(:at, at)
bx = +"abcdef"
[0x5a].pack("XC", buffer: bx)
show(:back, bx)

# a frozen buffer raises, a non-String buffer raises TypeError
begin
  [1].pack("C", buffer: "frozen".freeze)
rescue FrozenError => e
  p [:frozen, e.class]
end
begin
  [1].pack("C", buffer: 5)
rescue TypeError => e
  p [:type, e.message]
end

# typed receivers: Integer, Float and String arrays, and a mixed one
ib = "".b; [1, 2, 3].pack("C3", buffer: ib); show(:ints, ib)
fb = "".b; [1.5, 2.5].pack("e2", buffer: fb); show(:floats, fb)
sb = "".b; ["ab", "cd"].pack("A3A3", buffer: sb); show(:strs, sb)
mb = "".b; [1, "z", 2.0].pack("Ca1e", buffer: mb); show(:mixed_ary, mb)

# a buffer held in an ivar, grown across calls, and another name for it
class Enc
  def initialize = (@buf = "".b)
  def put(v) = ([v].pack("N", buffer: @buf); self)
  attr_reader :buf
end
e = Enc.new
alias_buf = e.buf
100.times { |i| e.put(i) }
p e.buf.bytesize, alias_buf.equal?(e.buf), e.buf.byteslice(396, 4).unpack1("N")

# a long message built a pack at a time
msg = "".b
20_000.times { |i| msg << 0x59; [i * 0.5].pack("E", buffer: msg) }
p msg.bytesize, msg.byteslice(9 * 19_999 + 1, 8).unpack1("E"), msg.encoding
