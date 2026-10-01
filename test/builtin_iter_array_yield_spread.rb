# combination, permutation and their repeated forms, product, zip, tap and
# then yield one value per step, and a block of two or more parameters
# spreads it when it is an Array, as a yield of one Array does: `|q, r|`,
# `_1, _2`, a rest, an optional and a post all take its elements.

a = [3, 1]
a.combination(2) { |q, r| p [q, r] }
a.combination(2) { p [_1, _2] }
a.permutation(2) { |*r| p r }
a.permutation { |q, *r| p [q, r] }
a.repeated_combination(2) { |q, r = 5, s = 6| p [q, r, s] }
a.repeated_permutation(1) { |q, r| p [q, r] }
[1.5, 2.5].combination(2) { |q, r| p q + r }
[3, "s"].permutation(2) { |q, r| p [r, q] }
a.combination(2) { |q, k: 4| p [q, k] }
a.combination(1) { |q, | p q }

a.product([7]) { |x, y| p [x, y] }
a.product([7]) { |*r| p r }
a.product([7]) { p [_1, _2] }
a.product([7], [8]) { |x, *r, z| p [x, r, z] }
g = [3, "s"]
g.product([7]) { |x, y| p [x, y] }
g.product([7]) { |*r| p r }
g.product([7], [8]) { |x, y, z| p [x, y, z] }
b = [[3, 1], 2][ARGV.size]
b.product([7]) { |x, y| p [x, y] }
b.combination(1) { |x, y| p [x, y] }

a.zip([7]) { |x, y, z| p [x, y, z] }
a.zip([7]) { |*r| p r }
a.zip([7]) { |x, y = 5| p [x, y] }

a.tap { |q, r| p [q, r] }
a.tap { |*r| p r }
p(a.then { |q, r| q + r })
p(a.then { _1 * _2 })
p("s".then { |*r| r })
p(nil.then { |q, r = 5| [q, r] })

# the in-place filters over rows
rows = [[1, "a"], [2, "b"]]
p(rows.dup.select! { |n, s| n > 1 && s == "b" })
p(rows.dup.keep_if { |n, s| s == "a" })
p(rows.dup.delete_if { |n, s| n > 1 })
p(rows.dup.reject! { |n, s| s == "a" })
p(rows.dup.map! { |n, s| s * n })
p([[1, "a"], 2].select! { |q, r| r })

# a boxed Hash's each_key and each_value spread the key or the value
h = [{ 3 => [1, 2] }, 1][ARGV.size]
h.each_value { |q, r| p [q, r] }
h.each_key { |q, r| p [q, r] }

# map! and sort_by! on an Array read back from a box typed by its values
class Bx
  attr_reader :got
  def each = (@got = yield)
end
bx = Bx.new
bx.each { [3, 1] }
p(bx.got.map! { |q| q * 2 })
p(bx.got.sort_by! { |q| q })
