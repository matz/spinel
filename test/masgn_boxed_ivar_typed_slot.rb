# An instance variable another write already types keeps that slot when it
# is also destructured from a boxed value (optcarrot's PPU @io_addr), and an
# untyped or nil one takes the boxed elements
class Lut
  def initialize
    @addr = 0
    @fetched = nil
    @lut = (0..3).map { |i| [i * 16, [i, i + 1], i] }
  end

  def fetch(i)
    @addr, @fetched, = @lut[i]
    @addr & 0x1f
  end

  def fetched = @fetched
end
l = Lut.new
p l.fetch(1), l.fetched
p l.fetch(3), l.fetched

# an Array slot another write types takes a boxed value's element boxed, as
# a plain write of a boxed value makes it: a typed store would read an
# element of another kind as nil
class Rows
  def initialize = (@w = [1])
  def take(v) = (@h, @w = v)
  attr_reader :w, :h
end
r = Rows.new
r.take([[5, 6], 3][ARGV.size]); p r.w, r.h
r.take([3, [[7], [8]]][ARGV.size + 1]); p r.w, r.h
@m = [1]
v = [[5, 6], 3][ARGV.size]; @n, @m = v
p @m, @n
