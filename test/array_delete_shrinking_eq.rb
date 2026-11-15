# spinel: gc-stress
# An == that shrinks the Array while delete walks it: delete stops at the
# new end, never lengthens the Array past what == left, and answers the
# last equal element, nil when nothing moved, or the argument when nothing
# was equal but the Array still had to shrink. The same holds for an Array
# that reaches delete as a boxed value, and for an @ivar that holds objects
# of one class.
class Cut
  attr_reader :n
  def initialize(n, arr, cut, hit) = (@n = n; @arr = arr; @cut = cut; @hit = hit)
  def ==(o)
    @arr.pop(@cut) if @cut > 0
    @hit
  end
  def inspect = "C#{n}"
end

def cuts(specs)
  arr = []
  specs.each_with_index { |(cut, hit), i| arr << Cut.new(i + 1, arr, cut, hit) }
  arr
end

SPECS = [
  [[0, false], [0, false], [2, false]],
  [[0, true], [0, false], [2, false]],
  [[0, true], [0, false], [1, false], [0, false]],
  [[0, false], [1, false], [0, false]],
  [[0, false], [0, true], [2, false], [0, false]],
]

SPECS.each do |specs|
  arr = cuts(specs)
  r = arr.delete(:k)
  p [r, arr]
end

SPECS.each do |specs|
  arr = cuts(specs)
  b = [arr, 1]
  r = b[0].delete(:k)
  p [r, arr]
end

class Item
  attr_reader :n
  def initialize(n, box, cut, hit) = (@n = n; @box = box; @cut = cut; @hit = hit)
  def ==(o)
    @box.cut(@cut) if @cut > 0
    @hit
  end
end

class Box
  def initialize = @all = []
  def add(n, cut, hit) = @all << Item.new(n, self, cut, hit)
  def cut(k) = k.times { @all.pop }
  def drop(x) = @all.delete(x)
  def ns = @all.map(&:n)
end

SPECS.each_with_index do |specs, k|
  bx = Box.new
  specs.each_with_index { |(cut, hit), i| bx.add(i + 1, cut, hit) }
  r = bx.drop(Item.new(9, bx, 0, false))
  p bx.ns
  p r&.n if k > 0
end
