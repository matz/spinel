# A block's value waits in a temp until its caller takes it. A value that
# leaves through an `ensure` (a `next`, or the tail inside the begin) waits
# there while the ensure runs, and an ensure that allocates can collect:
# the temp is a GC root, so the value survives. Checked with
# SPINEL_GC_STRESS=1 and 2 (several of these went wrong without it).

def junk
  x = []
  300.times { |i| x << [i, i.to_s] }
  x.size
end
Pt = Struct.new(:v)
def my_map(a)
  r = []
  a.each { |x| r << yield(x) }
  r
end

a = (1..30).to_a

# an inlined yield's carrier: flat_map, group_by, a method that yields
p(a.flat_map { |x| begin; next [[x, x.to_s]] if x.even?; [x]; ensure; junk; end }.last(3))
p(a.group_by { |x| begin; next [x % 3, x.to_s] if x.even?; x % 3; ensure; junk; end }.keys.last(3))
p(my_map(a) { |x| begin; next [x, x.to_s] if x.even?; [x]; ensure; junk; end }.last(3))
p(my_map(a) { |x| begin; next Pt.new(x * 11) if x.even?; Pt.new(0); ensure; junk; end }.map(&:v).last(4))
p(my_map(a) { |x| begin; next "s#{x}" * 2 if x.even?; "t#{x}"; ensure; junk; end }.last(3))

# a sort_by key (unique keys: sort_by keeps no order for ties)
p(a.sort_by { |x| begin; next [x % 7, x] if x.even?; [x % 7, -x]; ensure; junk; end }.first(8))

# map! over a String Array
b = a.map(&:to_s)
b.map! { |x| begin; next x * 2 if x.size == 1; x; ensure; junk; end }
p b.first(4), b.last(2)

# an Enumerator's with_index
e = a.map
p(e.with_index { |x, i| begin; next [x, i.to_s] if i.even?; x; ensure; junk; end }.last(3))

# merge's and fetch's blocks
h = {1 => 2, 3 => 4, 5 => 6, 7 => 8, 9 => 10}
p(h.merge({3 => 20, 7 => 40, 9 => 11}) { |k, o, n| begin; next [o, n.to_s] if n > 10; [k]; ensure; junk; end })
p(h.fetch(4) { |k| begin; next [k, k.to_s] if k > 2; [0]; ensure; junk; end })
