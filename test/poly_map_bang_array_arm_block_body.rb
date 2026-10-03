# map! on a poly receiver that is a builtin array at run time, with Set
# required so the call dispatches on the receiver's class: the array arm
# runs the whole block for every element, its earlier statements and the
# temps its value needs included, with the block parameter bound.
require 'set'
def mk(n); [n, n + 1]; end

m = [[1, 2], [3, 4]]
m[1].map! { |x| x * mk(x).length }
p m                                       # [[1, 2], [6, 8]]

m = [[1, 2], [3, 4]]
m[0].map! { |x| y = x + 1; y * 10 }
p m                                       # [[20, 30], [3, 4]]

m = [["a", "b"], ["c"]]
m[0].map! { |s| t = s + s; u = t.upcase; u + mk(1).length.to_s }
p m                                       # [["AA2", "BB2"], ["c"]]

# the Set arms of the same switch are as they were
h = { a: Set.new([1, 2, 3]) }
p h[:a].map { |x| x }                     # [1, 2, 3]
s2 = Set.new([1, 2])
hs = { b: s2 }
hs[:b].map! { |x| x + 5 }
p s2.to_a.sort                            # [6, 7]
