# A block-taking with_index on an Enumerator at a method's tail answers what
# its method makes of the block, as it does anywhere else: map's and
# select's collection, each's, each_slice's or reverse_each's receiver.
# The method answered nil when the receiver was boxed or the walk was one
# the statement form takes.
def mw(a) = a.map.with_index { |x, i| [x, i] }
def sw(a) = a.select.with_index { |x, i| i.odd? }
def rw(a) = a.reject.with_index { |x, i| i.odd? }
def cw(a) = a.collect.with_index(1) { |x, i| i }
def fmw(a) = a.filter_map.with_index { |x, i| x if i > 0 }
def ewm(a) = a.each_with_index.map { |x, i| [x, i] }
def esm(a) = a.each_slice(1).map { |s| s.size }
def ewo(a) = a.each.with_object([]) { |x, acc| acc << x }
def mwo(a) = a.map.with_object(:k) { |x, o| [x, o] }
def stored(a) = (e = a.map; e.with_index { |x, i| x })
def ebi(a) = a.each.with_index { |x, i| x }
[[1, 2], [1.0, nil], %w[p q]].each do |arr|
  p [mw(arr), sw(arr), rw(arr), cw(arr), fmw(arr), ewm(arr), esm(arr), ewo(arr), mwo(arr), stored(arr), ebi(arr)]
end
def u_slice(a) = a.each_slice(2).with_index { |s, i| s }
def u_cons(a) = a.each_cons(2).with_index { |s, i| s }
def u_rev(a) = a.reverse_each.with_index { |x, i| x }
def u_entry(a) = a.each_entry.with_index { |x, i| x }
def v_slice(a) = (r = a.each_slice(2).with_index { |s, i| s }; r)
def v_rev(a) = (r = a.reverse_each.with_index { |x, i| x }; r)
def su_slice(a) = (e = a.each_slice(2); e.with_index(1) { |s, i| s })
p u_slice([1, 2, 3]), u_cons([1, 2, 3]), u_rev([1, 2]), u_entry([1, 2]), v_slice([4, 5, 6]), v_rev([7, 8]), su_slice([1, 2, 3])
p u_slice([1.0, nil, "x"]), u_rev([nil, :s])
def u_ewi(a) = a.each_with_index.with_index { |(x, j), i| x }
p u_ewi([5, 6]), u_ewi([1.5, nil])
# block shapes: destructuring, fewer params, an offset, next, break, a Hash
def d1(a) = a.each_slice(2).with_index { |(x, y), i| [x, y, i] }
def d2(a) = a.map.with_index { |x| x }
def d3(a) = a.map.with_index { 7 }
def d4(a) = a.map.with_index(10) { |x, i| i }
def d5(a) = a.map.with_index { |x, i| next :skip if i == 0; x }
def d6(a) = a.map.with_index { |x, i| break :early if i == 1; x }
def d7(h) = h.map.with_index { |(k, v), i| [k, v, i] }
def d8(h) = h.each.with_index { |(k, v), i| k }
def d9(a) = a.each_slice(2).map.with_index { |s, i| s.size + i }
def d10(s) = s.each_char.map.with_index { |c, i| c * i }
p d1([1, 2, 3]), d2([1, :a]), d3([1, :a]), d4([1, :a]), d5([1, :a, 2]), d6([1, :a, 2])
p d7({ a: 1, b: 2 }), d8({ a: 1 })
p d1([1.5, nil]), d4([2.5])

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
