# map!/collect!.with_index whose block gives a value of another kind
# widens the Array, as a plain map! block does.
a = [1, 2]
a.map!.with_index { |x, i| x * 10 + i }
p a
b = [1, 2]
b.map!.with_index { |x, i| i == 0 ? :w : x }
p b
c = [1, 2]
c.map!.with_index { |x, i| i == 0 ? "s" : x }
p c
e = [1, 2]
e.map!.with_index(1) { |x, i| i == 1 ? 1.5 : x }
p e
f = [1, 2]
f.collect!.with_index { |x, i| i == 0 ? nil : x }
p f
p f.map(&:class)

def tag_first(arr)
  arr.map!.with_index { |x, i| i == 0 ? :first : x }
  arr
end
p tag_first([5, 6, 7])

class Box
  def initialize; @items = [1, 2, 3]; end
  def mark
    @items.map!.with_index { |x, i| i.odd? ? "odd" : x }
    @items
  end
end
p Box.new.mark

S = Struct.new(:list)
s = S.new([3, 4])
s.list.map!.with_index { |x, i| i == 1 ? :two : x }
p s.list

g = %w[a b]
g.map!.with_index { |x, i| i == 0 ? 7 : x }
p g
