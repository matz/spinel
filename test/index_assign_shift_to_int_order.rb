# `@s[1 << o] = v` and `@s[1 << o] += v` read @s before the index runs
# o.to_int, and a to_int that assigns @s leaves the store in the Array
# read first.
class O
  def initialize(k) = @k = k
  def to_int
    @k.swap
    1
  end
end

class K
  attr_reader :s
  def initialize = @s = [0, 0, 0]
  def swap = @s = [7, 7, 7]
  def store(o) = (@s[1 << o] = 5; @s)
  def bump(o) = (@s[1 << o] += 1; @s)
end

k = K.new
first = k.s
p k.store(O.new(k))
p first

k = K.new
first = k.s
p k.bump(O.new(k))
p first
