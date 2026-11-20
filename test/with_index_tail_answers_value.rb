# A block-taking with_index at a method's tail answers its value where the
# value form knows it: the collection of a map, collect, select, filter or
# reject made right there, over any receiver (a boxed one too), and the
# receiver of an each over an Array or an Integer Range.
# The method answered nil where the statement form took the walk.
def mw(a) = a.map.with_index { |x, i| [x, i] }
def sw(a) = a.select.with_index { |x, i| i.odd? }
def rw(a) = a.reject.with_index { |x, i| i.odd? }
def cw(a) = a.collect.with_index(1) { |x, i| i }
def fw(a) = a.filter.with_index { |x, i| i > 0 }
def fmw(a) = a.filter_map.with_index { |x, i| x if i > 0 }
def ewm(a) = a.each_with_index.map { |x, i| [x, i] }
def ewo(a) = a.each.with_object([]) { |x, acc| acc << x }
[[1, 2], [1.0, nil], %w[p q]].each do |arr|
  p [mw(arr), sw(arr), rw(arr), cw(arr), fw(arr), fmw(arr), ewm(arr), ewo(arr)]
end

# typed receivers
def tmw(a) = a.map.with_index { |x, i| x * i }
def tsw(a) = a.select.with_index { |x, i| i.odd? }
def tew(a) = a.each.with_index { |x, i| x }
p tmw([1, 2, 3]), tsw([1, 2, 3]), tew([1, 2])

# block shapes: fewer params, none, an offset, next, break, a Hash
def d2(a) = a.map.with_index { |x| x }
def d3(a) = a.map.with_index { 7 }
def d4(a) = a.map.with_index(10) { |x, i| i }
def d5(a) = a.map.with_index { |x, i| next :skip if i == 0; x }
def d6(a) = a.map.with_index { |x, i| break :early if i == 1; x }
def d7(h) = h.map.with_index { |(k, v), i| [k, v, i] }
def d8(h) = h.each.with_index { |(k, v), i| k }
p d2([1, :a]), d3([1, :a]), d4([1, :a]), d5([1, :a, 2]), d6([1, :a, 2])
p d7({ a: 1, b: 2 }), d8({ a: 1 }), d4([2.5])

# an Integer Range's each.with_index answers the Range, boxed where the
# value is (a method's tail, a local)
def f(n)
  (1..n).each.with_index { |x, i| puts x }
end
p f(2)
def g(n) = (1..n).each.with_index { |x, i| x }
p g(3)
r = (1..3).each.with_index { |x, i| x }
p r
def h(n) = (2..n).each.with_index(5) { |x, i| x * i }
p h(4)
