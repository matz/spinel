# getbyte, binmode, set_encoding, close_read and close_write on an IO read
# back out of an Array or a boxed instance variable, and respond_to? for
# each, as on a typed IO.
def pair
  r, w = IO.pipe
  w.write("ab\n")
  [r, w]
end

r, w = pair
p r.respond_to?(:getbyte)
io = pair
p io[0].getbyte
p io[0].getbyte
p io[0].binmode.class
p io[0].set_encoding(Encoding::BINARY).class
p io[0].set_encoding("UTF-8", "UTF-8").class
p io[0].close_read
p io[0].closed?
p io[1].close_write
p io[1].closed?

e = pair
e[1].close
p e[0].getbyte, e[0].getbyte, e[0].getbyte, e[0].getbyte

%i[getbyte binmode set_encoding close_read close_write].each do |m|
  p [m, pair[0].respond_to?(m)]
end

class Holder
  def initialize(io) = @io = io
  def io = @io
  def take(x) = @io = x
end
h = Holder.new(pair[1])
h.take(pair[1]) if false
h2 = Holder.new(1)
p h.io.close_write
p h.io.respond_to?(:close_write)
p h2.io
