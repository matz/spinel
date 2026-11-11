# A multiple assignment converts the object through the to_ary it has
# at that point: a class reopened between two assignments answers the
# second one with its new version, and a method called before and after
# the reopen, with both conversions and an explicit call in it, meets
# each version in turn.

class Box
  def to_ary = [1, 2]
end

o = Box.new
a, b = o
p [a, b]

class Box
  def to_ary = [3, 4]
end
a, b = o
p [a, b]

class Cell
  def to_ary = [:a, :b]
end

def both(o)
  a, b = o
  [a, b, o.to_ary]
end

c = Cell.new
p both(c)
class Cell
  def to_ary = [:c, :d]
end
p both(c)
