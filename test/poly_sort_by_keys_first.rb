# spinel: gc-stress
# sort_by on a value only known at run time (a boxed Array, Hash or Range, a
# `&f` forwarded to it, an Array reaching a program class's yielding
# sort_by) asks the block for every element first, in order, and orders the
# keys after, as CRuby does. A pair of keys that do not compare raises only
# then, so the block has seen every element; and a block that raises on a
# later element raises that error, not the comparison's. The keys sort in
# O(n log n): a long reverse-ordered input finishes at once.
class Pile
  def sort_by = [yield(0)]
end
# a Numeric of the program's with its own coerce and <=>
class Num < Numeric
  def initialize(f) = @f = f
  def to_f = @f
  def coerce(o) = [o.to_f, @f]
  def <=>(o) = @f <=> o.to_f
end

# a <=> whose answer is no number: Comparable refuses it as CRuby's rb_cmpint asks
class StrCmp
  def <=>(o) = "x"
end

# (which pair of keys a sort compares first is CRuby's own and differs by
# platform, and so does the comparison error's message: only its kind)
def t
  yield
rescue StandardError => e
  puts e.message.start_with?("comparison of") ? "#{e.class}: comparison failed" : "#{e.class}: #{e.message}"
end

# the pre-existing report, a proc forwarded to a boxed Array
f = proc { |x| puts "k #{x.inspect}"; x }
v = ARGV.size > 5 ? 1 : [3, "a", 1]
t { p v.sort_by(&f) }
w = ARGV.size > 5 ? 1 : [3, 1, 2]
p w.sort_by(&f)

# a literal block, and a method forwarding its block
t { p(v.sort_by { |x| puts "b #{x.inspect}"; x }) }
def srt(xs, &b) = xs.sort_by(&b)
t { p(srt(v) { |x| puts "m #{x.inspect}"; x }) }

# a Hash and a Range
g = proc { |k, val| puts "h #{val.inspect}"; val }
h = ARGV.size > 5 ? 1 : {a: 3, b: "x", c: 1}
t { p h.sort_by(&g) }
h2 = ARGV.size > 5 ? 1 : {a: 3, b: 2, c: 1}
p h2.sort_by(&g)
r = ARGV.size > 5 ? 1 : (1..3)
t { p(r.sort_by { |x| puts "r #{x}"; x == 2 ? "s" : x }) }
p(r.sort_by { |x| -x })

# a program class owning a yielding sort_by, an Array reaching the call
d = ARGV.size > 5 ? Pile.new : [3, "a", 1]
t { p(d.sort_by { |x| puts "d #{x.inspect}"; x }) }
p(Pile.new.sort_by { |x| x + 5 })

# a nil key, a NaN, a Float with an Integer
n = ARGV.size > 5 ? 1 : [2, 5, 1]
t { p(n.sort_by { |x| puts "n #{x}"; x == 5 ? nil : x }) }
fl = ARGV.size > 5 ? 1 : [2.5, 1, 3.5, 2]
p fl.sort_by { |x| x }
nan = ARGV.size > 5 ? 1 : [1.0, 0.0, 2.0]
t { p(nan.sort_by { |x| puts "f #{x}"; x == 0.0 ? Float::NAN : x }) }

# keys that compare as equal or by their own class: the boxed arm orders them
# where it used to raise "comparison of X with X failed"
o = Object.new
h = {b: 2}
kinds = [
  [:nils, [nil, nil, nil]],
  [:true_true, [true, true]],
  [:false_false, [false, false]],
  [:same_obj, [o, o]],
  [:same_range, [(1..2), (1..2)]],
  [:equal_hashes, [{a: 1}, {a: 1}]],
  [:one_hash_twice, [h, h]],
  [:num_beside_ints, [Num.new(2.5), 1, Num.new(0.5), 3]],
  [:strcmp, [StrCmp.new, StrCmp.new]],
  [:true_false, [true, false]],
  [:sym_str, [:a, "a"]],
  [:int_float, [3, 1.5, 2, 0.5]],
]
kinds.each do |name, ks|
  w = ARGV.size > 5 ? 1 : (0...ks.size).to_a
  t { p [name, w.sort_by { |i| ks[i] }] }
end

t { p [1, 2].sort_by { |x| StrCmp.new } }

# the block's own error on a later element wins over the comparison's
e = ARGV.size > 5 ? 1 : [3, "a", 1, 4]
t { p(e.sort_by { |x| puts "e #{x.inspect}"; raise "stop at #{x}" if x == 4; x }) }

# keys of other kinds that compare: Strings, Arrays, a Float against an Integer
s = ARGV.size > 5 ? 1 : %w[pear fig apple kiwi]
p s.sort_by { |x| x }
p s.sort_by { |x| [x.size, x] }
p s.sort_by { |x| -x.size.to_f - 0.5 }

# equal keys keep their order
q = ARGV.size > 5 ? 1 : [[1, :a], [0, :b], [1, :c], [0, :d], [1, :e]]
p q.sort_by { |x| x[0] }

# a long reverse-ordered input, the keys fresh Strings (collected between answers)
big = ARGV.size > 5 ? 1 : (1..3000).to_a.reverse
k = big.sort_by { |x| x }
p [k[0], k[1], k[-1], k.size]
big2 = ARGV.size > 5 ? 1 : (1..400).to_a.reverse
k2 = big2.sort_by { |x| "key #{1000 + x}" }
p [k2[0], k2[1], k2[-1], k2.size]
