# `a[i, n] = src` through a poly receiver holding a typed array splices into
# that array, as Array#[]= does: the caller's array sees it. The source here
# is a poly Array (`bytes.first(n)` on a boxed value).
class Flash
  BANK = 4

  def initialize(chips)
    low = Array.new(BANK * 2, 0xff)
    chips.each { |off, data| place(low, off, data) }
    @low = low
  end

  attr_reader :low

  def place(flash_data, offset, bytes)
    bytes = bytes.first(BANK)
    flash_data[offset, bytes.length] = bytes
  end
end

class Slots
  def initialize = @slots = []
  def place(sid) = (@slots << sid; self)
end

def pick(i) = i > 5 ? Slots.new : Flash.new([[0, [1, 2]]])
def widen(h) = h.is_a?(Integer) ? h : h

p Flash.new([[0, [1, 2, 3, 4, 5]], [4, [9]]]).low
x = widen(pick(0))
x.place(3) if x.is_a?(Slots)

# floats and strings likewise
def put(arr, src) = (arr[1, src.length] = src)
fs = [0.5, 1.5, 2.5]
put(fs, [9.5].first(1))
p fs
ss = %w[a b c]
put(ss, %w[z].first(1))
p ss

# an element the array's kind cannot hold still turns the poly local into a
# general Array
v = [1, 2, 3]
w = [v, "s"][0]
w[0, 1] = ["s"]
p w
