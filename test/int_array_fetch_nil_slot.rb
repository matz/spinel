# fetch of an Integer or Float Array's slot that holds nil (a `map`
# block's `v if c` answers one) answers nil, as [] does, inside a method
# and at the top level, with or without a default
def main
  a = [3, 4, 5].map { |v| v if v != 4 }
  p a.fetch(1), a.fetch(0), a.fetch(-2), a.fetch(5, :d)
  f = [1.5, 2.5].map { |v| v if v < 2 }
  p f.fetch(1), f.fetch(-1), f.fetch(0)
  x = a.fetch(2)
  p x + 1 if x
end
main
b = [3, 4].map { |v| v if v > 3 }
p b.fetch(0), b.fetch(1, 9), b.fetch(0, 9)
# a nil values_at answers for an index outside the Array is one too
v = [1, 2, 3].values_at(0, 5)
p v.fetch(1), v.fetch(0)
w = [1.5].values_at(0, 3)
p w.fetch(1)
# a block fetch hands the missing index: the parameter is the index, never
# nil, and an index inside answers the element, nil or not
m = [3, 4, 5].map { |x| x if x != 4 }
p m.fetch(1) { |i| i * 10 }, m.fetch(7) { |i| i * 10 }
g = [1.5, 2.5].map { |x| x if x > 2 }
p g.fetch(0) { |i| i + 0.5 }, g.fetch(-5) { |i| "missing #{i}" }
