# spinel: gc-stress
# A statement `arr.each.with_index(off) { |x, i| }` over an Array walks the
# receiver itself, as the expression form does: no drained Enumerator copy
# (an Integer Range's chain still drains one). So a block that grows or shrinks the array sees
# it as CRuby does, and break, next, return, offsets, destructuring and
# every element kind answer as before. It keeps the arrays' headers as `each`
# does, and polls as `each` does.
def dot(a, b) = (s = 0.0; a.each.with_index { |x, i| s += x * b[i] }; s)
def offs(a) = (r = []; a.each.with_index(1) { |x, i| r << [x, i] }; r)
def neg(a) = (r = []; a.each.with_index(-2) { |x, i| r << i }; r)
def ints(a) = (s = 0; a.each.with_index { |x, i| s += x * i }; s)
def strs(a) = (r = []; a.each.with_index { |w, i| r << "#{i}:#{w}" }; r)
def objs(a) = (r = 0; a.each.with_index { |o, i| r += o.size + i }; r)
def poly(a) = (r = []; a.each.with_index { |v, i| r << [v.class, i] }; r)
def rng(n) = (s = 0; (0...n).each.with_index { |x, i| s += x * 10 + i }; s)
def rng_off(n) = (r = []; (1..n).each.with_index(5) { |x, i| r << [x, i] }; r)
def brk(a) = a.each.with_index { |x, i| break [x, i] if i == 2 }
def nxt(a) = (s = 0; a.each.with_index { |x, i| next if i.odd?; s += x }; s)
def ret(a) = (a.each.with_index { |x, i| return i if x > 2 }; -1)
def grow(a) = (r = []; a.each.with_index { |x, i| a << 99 if i == 0; r << x }; r)
def shrink(a) = (r = []; a.each.with_index { |x, i| a.pop if i == 0; r << x }; r)
def value(a) = a.each.with_index { |x, i| x }
def one(a) = (r = []; a.each.with_index { |x| r << x }; r)
def none(a) = (n = 0; a.each.with_index { n += 1 }; n)
def destr(a) = (r = []; a.each.with_index { |(p, q), i| r << p + q + i }; r)
def splat(a) = (r = []; a.each.with_index { |*args| r << args }; r)
def nilv(a) = (r = []; a.each.with_index { |x, i| r << (x.nil? ? i : x) }; r)
def frz(a) = (s = 0.0; a.each.with_index { |x, i| s += x + i }; s)
def wiv(a) = (s = 0; a.each.with_index(a.size) { |x, i| s += i }; s)
p dot([1.0, 2.0, 3.0], [4.0, 5.0, 6.0]), offs([:a, :b]), neg([1, 2, 3]), ints([3, 4, 5])
p strs(%w[x y]), objs(["ab", "c"]), poly([1, "s", 2.5, nil]), rng(5), rng_off(3)
p brk([10, 20, 30, 40]), nxt([1, 2, 3, 4]), ret([1, 2, 3, 4]), ret([0]), grow([1, 2]), shrink([1, 2, 3])
p value([1, 2]), one([7, 8]), none([1, 2, 3]), destr([[1, 2], [3, 4]]), splat([1, 2]), nilv([1, nil, 3])
p frz([1.5, 2.5].freeze), wiv([5, 6, 7]), dot([], [])
big = Array.new(70_000) { |i| i * 0.5 }
p dot(big, big)
s = 0
[[1, 2], [3]].each.with_index { |row, i| row.each.with_index { |x, j| s += x * (i + 1) * (j + 1) } }
p s
def sbrk(a)
  r = 0
  a.each.with_index { |x, i| break if i == 2; r += x }
  r
end
p sbrk([5, 6, 7, 8])

# inside a while that keeps the arrays' headers, storing, growing, other kinds
def nested(m, v)
  s = 0.0
  k = 0
  while k < m.size
    row = m[k]
    row.each.with_index { |x, j| s += x * v[j] }
    k += 1
  end
  s
end
def stores(a, out) = (a.each.with_index { |x, i| out[i] = x * 2.0 }; out)
def stores_short(a, out) = (a.each.with_index { |x, i| out[i] = x * 2.0 }; out)
def grow_in(a) = (n = 0; a.each.with_index { |x, i| a << x if i < 2; n += 1 }; [n, a.size])
def strs(a) = (r = 0; a.each.with_index { |s, i| r += s.size * i }; r)
def objs(a) = (r = 0; a.each.with_index { |o, i| r += o.first + i }; r)
def frozen_out(a, out) = (a.each.with_index { |x, i| out[i] = x }; out)
m = [[1.0, 2.0], [3.0, 4.0], [5.0, 6.0]]
p nested(m, [0.5, 0.25])
p stores([1.0, 2.0, 3.0], [0.0, 0.0, 0.0]), stores_short([1.0, 2.0, 3.0], [0.0])
p grow_in([1, 2, 3]), strs(%w[ab c d]), objs([[1], [2, 3]])
begin; p frozen_out([1.0], [0.0].freeze); rescue => e; p e.class; end
nb = Array.new(3) { |i| i * 1.0 }; nb[1] = nil
begin; p nested([[1.0, 1.0, 1.0]], nb); rescue => e; p e.class; end

# a nil receiver held in a typed slot raises as each does; a next in the walk
# inside a collecting block leaves the walk's pass, not the block's
class H
  def initialize(set) = (@a = [1.0, 2.0] if set)
  def run = (s = 0.0; @a.each.with_index { |x, i| s += x * i }; s)
end
def loc(flag)
  a = flag ? [1, 2, 3] : nil
  s = 0
  a.each.with_index { |x, i| s += x + i }
  s
end
p H.new(true).run, loc(true)
[-> { H.new(false).run }, -> { loc(false) }].each do |f|
  p f.call
rescue NoMethodError => e
  p [:NoMethodError, e.message[0, 40]]
end
def o3(rows, arr)
  rows.map do |k|
    s = +""
    arr.each.with_index { |x, i| next 5 if i == 0; s << x.to_s }
    s
  end
end
def o4(rows, arr)
  rows.map do |k|
    arr.each.with_index { |x, i| next :sym if i == 0; k + x }
    k.to_s
  end
end
def o5(rows, arr) = rows.select { |k| arr.each.with_index { |x, i| next if x > k }; k.odd? }
p o3([1, 2], [7, 8]), o4([1, 2], [3, 4]), o5([1, 2, 3], [2, 5])

# begin / rescue / ensure around a next in the walk, inside a method with a
# rescue of its own; a Range walk at a method's tail (it builds and runs)
def parse(lines)
  out = []
  lines.each.with_index(1) do |line, no|
    begin
      next if line.empty?
      out << Integer(line)
    rescue ArgumentError
      out << -no
    end
  end
  out
rescue IOError
  []
end
r = parse(Array.new(2000) { |i| i % 3 == 0 ? "" : (i % 5 == 0 ? "x" : i.to_s) })
p r.size, r.sum
begin; Integer("bad"); rescue ArgumentError => e; p e.class; end
def f1(a)
  r = []
  a.each.with_index do |x, i|
    begin
      next if i == 1
      r << x
    ensure
      r << :ens
    end
    r << :after
  end
  r
end
p f1([1, 2, 3])
def tail_rng(n)
  (1..n).each.with_index { |x, i| print x, i }
end
tail_rng(2)
puts

# a chain with a `&.` keeps the drained path, whose guard answers nil
class SafeBox
  def initialize(on) = (@a = [1.5, 2.5] if on)
  def each_wi2 = (r = []; @a&.each&.with_index { |x, i| r << i }; r)
end
p SafeBox.new(true).each_wi2, SafeBox.new(false).each_wi2
def chars_wi(s) = (r = []; s&.each_char&.with_index { |ch, i| r << ch * i }; r)
p chars_wi("ab"), chars_wi(nil)
