# Array#<< takes exactly one argument. As a statement, `a.<<(1, 2)` and
# `a.<<(*xs)` with xs of length != 1 raise ArgumentError and leave the
# Array unchanged; one-element splats push that element.
def try
  yield
rescue ArgumentError => e
  e.message
end

ints = [1]
p try { ints.<<(2, 3); :pushed }
p ints
strs = ["a"]
p try { strs.<<("b", "c"); :pushed }
p strs
flts = [1.5]
p try { flts.<<(2.5, 3.5); :pushed }
p flts
mix = [1, "a"]
p try { mix.<<(:s, nil); :pushed }
p mix
nest = [[1]]
p try { nest.<<([2], [3]); :pushed }
p nest
xs = [2, 3]
p try { ints.<<(*xs); :pushed }
p ints
none = []
p try { ints.<<(*none); :pushed }
p ints
one = [9]
ints.<<(*one)
p ints
ints << 10
p ints

class Box
  def initialize = @items = []
  def add2(a, b)
    @items.<<(a, b)
    :pushed
  rescue ArgumentError => e
    e.message
  end
  def items = @items
end
b = Box.new
p b.add2(1, 2)
p b.items
