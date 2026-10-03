# A `next` in a block that is run once where it is written, with no loop of
# its own, leaves that block with the `next`'s value: the block of
# instance_eval and instance_exec, of catch, of define_method and
# define_singleton_method, and of a Struct's to_h. Outside a loop the `next`
# came out as a C `continue` no compiler accepts; inside one it built, and
# the `continue` took the enclosing loop's next turn instead.
c = ARGV.length == 0

# instance_eval and instance_exec: with a value, without one, of each kind
p 5.instance_eval { next 0 if c; self * 2 }
p 5.instance_eval { next 0 unless c; self * 2 }
p "ab".instance_eval { next size if c; 0 }
p 5.instance_exec(2) { |n| next n + self if c; self }
p [1, 2].instance_eval { next first if c; last }
p nil.instance_eval { next 1 if c; 2 }
p :s.instance_eval { next if c; 2 }
p 5.instance_eval { next "s" if c; :sym }
p 5.instance_eval { next 1.5 if c; 2 }
p 5.instance_eval { if self > 3 then next :big else next :small end }

# in a loop the next ends the block, not the loop's turn
i = 0
out = []
while i < 3
  i += 1
  v = i.instance_eval { next -1 if self == 2 && c; self * 10 }
  out << v
  out << :after
end
p out
p [1, 2, 3].map { |x| x.instance_eval { next 0 if self == 2; self } + 100 }

# a captured local written before the next keeps the write
x = 0
5.instance_eval { x = self; next if c; x = 9 }
p x

# an ensure the next leaves runs first
g = 7.instance_eval do
  begin
    next self + 1 if c
  ensure
    puts "ensure"
  end
  0
end
p g

# catch: the next's value is the catch's, and a throw still works beside it
p catch(:t) { next 5 if c; throw :t, 1 }
p catch(:t) { |tag| next 6 if !c; throw tag, 1 }
p catch { |tag| next 7 if c; throw tag, 1 }
h = catch(:t) do
  [1, 2, 3].each { |e| next if e == 1; throw :t, e * 10 if !c }
  next :done if c
  :no
end
p h
p [1, 2, 3].map { |e| catch(:k) { next 0 if e == 2; throw :k, 9 if e == 3; e } + 100 }
y = 1
z = catch(:t) { y += 1; next y * 2 if c; y = 0 }
p y, z

# in a method, a proc and a lambda the statements after the block still run
def in_method(c)
  w = 3.instance_eval { next self * 2 if c; 0 }
  k = catch(:q) { next 1 if c; 2 }
  [w, k, :end]
end
p in_method(true), in_method(false)
pr = proc { |n| m = n.instance_eval { next self + 1 if c; 0 }; [m, :proc_end] }
p pr.call(4)
l = ->(n) { m = catch(:q) { next n * 2 if c; 0 }; [m, :lambda_end] }
p l.call(4)

# define_method and define_singleton_method: the block is the method's body,
# and its next ends the call
class Box
  def base = 3
  define_method(:dm) { next 0 if ARGV.length == 0; base * 2 }
  define_method(:dn) { |n| next :s if n > 1; base * 2 }
  define_method(:dk) { |n, k: 1| next k if n > 1; base * 2 }
  define_method(:dv) { |n| if n > 1 then next end; base }
  define_method(:dl) { |n| [1, 2, 3].map { |e| next 0 if e == n; e } }
  define_method(:dw) do |n|
    j = 0
    while j < 5
      j += 1
      next if j < n
      break
    end
    next j * 10 if n > 0
    -1
  end
  define_method(:de) do |n|
    begin
      next n * 2 if n > 1
    ensure
      puts "method ensure"
    end
    1
  end
  class << self
    define_method(:cm) { |n| next "neg" if n < 0; "pos" }
  end
end
b = Box.new
p b.dm, b.dn(1), b.dn(2), b.dk(2, k: 5), b.dv(1), b.dv(2), b.dl(2), b.dw(3), b.dw(0)
p b.de(1), b.de(2), Box.cm(-1), Box.cm(1)
b.define_singleton_method(:doubled) { next 0 if ARGV.length == 0; base * 2 }
b.define_singleton_method(:tripled) { |n| next :big if n > 1; base * 3 }
p b.doubled, b.tripled(1), b.tripled(2)

# the names an each over literals writes are methods too
class Unrolled
  [:a, :b].each { |v| define_method("m_#{v}") { |n| next "#{v} big" if n > 1; v.to_s } }
  [1, 2].each { |v| define_method("n_#{v}") { next v * 10 if ARGV.length == 0; v } }
  %w[x y].each do |v|
    define_method("s_#{v}") { |n| if n > 1 then next v * n end; v }
  end
end
ur = Unrolled.new
p ur.m_a(1), ur.m_a(2), ur.m_b(2), ur.n_1, ur.n_2, ur.s_x(2), ur.s_y(1), ur.s_y(3)

# a Struct's to_h: the next hands in the pair for that member
Pair = Struct.new(:x, :y)
s = Pair.new(1, 20)
t = s.to_h { |k, e| next [k, 0] if e > 5 && c; [k, e * 2] }
p t[:x], t[:y], t.size
u = s.to_h { |k, e| next [k.to_s, e] if c; ["", 0] }
p u["x"], u["y"], u.keys
i = 0
while i < 2
  i += 1
  r = Pair.new(i, 2).to_h { |k, e| next [k, 0] if e == 2; [k, e] }
  puts "turn #{i}: #{r[:x]} #{r[:y]}"
end
pt = proc { |n| r = s.to_h { |k, e| next [k, n] if e > 5; [k, e] }; r[:x] + r[:y] }
p pt.call(100)
Coord = Data.define(:a, :b)
d = Coord.new(a: 3, b: 4).to_h { |k, e| next [k, -e] if e > 3 && c; [k, e] }
p d[:a], d[:b]
