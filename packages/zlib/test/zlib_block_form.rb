# The block forms of Zlib::Inflate#inflate, Zlib::Deflate#deflate, #flush and
# #finish: each chunk of output goes to the block, and the call answers nil,
# as a program that bounds the inflated size reads it (a chunk at a time).
require "zlib"

text = (1..200).map { |i| "line #{i} of the text\n" }.join
deflated = Zlib::Deflate.deflate(text)

z = Zlib::Inflate.new
out = "".b
r = z.inflate(deflated) { |chunk| out << chunk }
p r, out == text.b, z.finished?
out << z.finish
p out == text.b
z.close

# fed in two pieces, with a block each time, then finish with a block
z = Zlib::Inflate.new
parts = []
p z.inflate(deflated[0, 20]) { |c| parts << c }
p z.inflate(deflated[20..]) { |c| parts << c }
p z.finish { |c| parts << c }
p parts.join == text.b, parts.all? { |c| c.is_a?(String) }
z.close

# a block that leaves the method early: the rest of the stream stays
def capped(deflated, cap)
  z = Zlib::Inflate.new
  out = "".b
  z.inflate(deflated) do |chunk|
    out << chunk
    return nil if out.bytesize > cap
  end
  out << z.finish
  out
ensure
  z&.close
end
p capped(deflated, 10), capped(deflated, 1 << 20) == text.b

d = Zlib::Deflate.new
buf = "".b
p d.deflate(text[0, 1000]) { |c| buf << c }
p d.deflate(text[1000..]) { |c| buf << c }
p d.flush { |c| buf << c }
p d.finish { |c| buf << c }
d.close
p Zlib::Inflate.inflate(buf) == text
