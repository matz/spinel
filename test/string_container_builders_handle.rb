# The Strings a String Array or a String-valued Hash gets from its builders are the Strings that went in:
# one object, so a change through any name shows in the container, and the other way round.

# sort with a comparator block, in place and copying: the elements move, none is copied
a = [+"pear", +"fig", +"apple"]
pear, fig, apple = a[0], a[1], a[2]
b = a.sort { |x, y| x <=> y }
p b[0].equal?(apple), b[1].equal?(fig), b[2].equal?(pear)
a.sort! { |x, y| y <=> x }
p a[0].equal?(pear), a[2].equal?(apple)
a[0] << "!"
p pear, b[2]

# merge's block value and transform_values: the block's String is the value
s = +"merged"
h = {"a" => +"1", "b" => +"2"}.merge({"a" => +"3"}) { |k, o, n| s }
p h["a"].equal?(s), h["b"]
t = {"a" => +"1"}.transform_values { |v| s }
p t["a"].equal?(s)
s << "?"
p h, t

# value? / has_value? with a String in hand, and index with a boxed needle
v = +"two"
g = {1 => +"one", 2 => v}
p g.value?(v), g.has_value?(+"two"), g.value?("zzz")
boxed = [v, 3].first
arr = [+"x", v]
p arr.index(boxed), arr.rindex(v)

# copies: values_at, fetch_values, cycle, take_while, uniq!, to_h, partition
w = [+"a", +"b", +"c"]
p w.values_at(2, 0)[0].equal?(w[2]), w.fetch_values(1)[0].equal?(w[1])
p w.cycle.first(4)[3].equal?(w[0])
p w.take_while { |e| e != "c" }[1].equal?(w[1])
u = [+"k", +"k", +"j"]
first = u[0]
u.uniq!
p u.size, u[0].equal?(first)
x = +"val"
p [["key", x]].to_h["key"].equal?(x)
S = Struct.new(:a)
st = S.new(x)
p st.to_h { |k, val| [k.to_s, val] }["a"].equal?(x)
pre, mid, post = "abc".partition(/b/)
mid << "!"
p [pre, mid, post], $&

# multiple assignment into an Array slot and a Hash slot, and a splat of a String Array into an Array
m = +"slot"
arr2 = [+"o", +"p"]
arr2[1], n = m, 1
p arr2[1].equal?(m)
hh = {"k" => +"v"}
hh["k"], n = m, 1
p hh["k"].equal?(m)
m << "?"
p arr2, hh
q = [*w, +"d"]
p q[0].equal?(w[0]), q.size

# the Strings a runtime builder makes are new, one handle each
lines = "x\ny\n".lines
lines[0] << "!"
p lines, "x\ny\n".lines
cs = "ab12".match(/(\w)(\d)/).captures
cs[0] << "!"
p cs
up = "ab".unpack("a1a1")
up[0] << "!"
p up
require "stringio"
rl = StringIO.new("a\nb\n").readlines
rl[0] << "!"
p rl

# values_at of a match boxes each group as its own String
md = "b1".match(/(\w)(\d)/)
va = md.values_at(1, 2, 0)
va[0] << "!"
va[2] << "?"
p va, md.values_at(1..2)[0] << "^", md.values_at(2)[0] << "&"

# splat push / unshift / insert / concat, Array(), `*=` and a boxed concat into a String Array
sp = ["x"]
xs = ["p", "q"]
sp.push(*xs)
sp.unshift(*xs)
sp.insert(1, *xs)
sp.concat(xs, xs)
p sp, Array("one"), sp.size
ra = [+"ab", +"c"]
ra[0] *= 2
ra[1] += "z"
p ra
hh2 = {a: 1, b: 2}
def two(k, v) = "#{k}:#{v}"
p((hh2.flat_map(&method(:two)) rescue $!.class))

# a lone splat-rest target, a boxed Array destructured into String slots, and a long block parameter name
lone = +"x"
*lr = lone
lr[0] << "!"
p lr, lone, lr[0].equal?(lone)
slots = ["s"]
nil_y = [nil, +"y"]
slots[0], slots[1] = nil_y
nil_y[1] << "!"
p slots, slots[1].equal?(nil_y[1])
long_names = [+"aa", +"b"].select { |a_very_long_block_parameter_name_that_a_fixed_buffer_would_cut_off_before_its_end_a_very_long_block_parameter_name_that_a_fixed_buffer_would_cut_off_before_its_end_a_very_long_block_parameter_name_that_a_fixed_buffer_would_cut_off_before_its_end_xxx| true }
long_names[0] << "!"
p long_names

# an in-place filter with a block that takes no parameter moves the handles, and a bound push / []= method object stores one
fl = [+"x", +"yy", +"z", +"ww"]
cnt = 0
fl.select! { cnt += 1; cnt.odd? }
fl[0] << "!"
p fl
fl2 = [+"a", +"b", +"c"]
k = 0
fl2.reject! { (k += 1) == 2 }
g2 = [+"p", +"q"]
g2.delete_if { false }
p fl2, g2
bound = ["x", "y"]
pm = bound.method(:push)
pz = +"z"
pm.call(pz)
pz << "!"
bound.method(:[]=).call(0, +"w")
p bound, bound.method(:[]).call(1)
