# Array.new(n) { } builds an Integer or Float result at length n and stores
# each value into its slot (it grew from an empty Array by pushes); object,
# boxed and nested results append as before; and a break, next or raise in
# the block.
class Qv
  attr_reader :v
  def initialize(v) = (@v = v)
end

p Array.new(4) { |i| i * i }, Array.new(3) { |i| i * 0.5 }, Array.new(0) { |i| i }
objs = Array.new(200) { |i| Qv.new(i) }
GC.start
p objs.size, objs[199].v, objs.sum(&:v)
t = Array.new(3) { |i| Array.new(2) { |j| i * 10 + j } }
p t, t[2][1]
p(Array.new(5) { |i| break :out if i == 3; i })
p Array.new(4) { |i| next -1 if i.odd?; i * 2 }
p Array.new(4) { |i| i.even? ? i : "s#{i}" }
p Array.new(3) { |i| i == 1 ? nil : i }
r = Array.new(3) { |i| "x" * (i + 1) }
r[0] << "!"
p r
p((begin; Array.new(4) { |i| raise "stop" if i == 2; i }; rescue => e; e.message; end))
k = 3
p Array.new(3) { |i| i + k }

# a block that leaves early pays only for what it filled: the slots made
# up front stop at a cap, and the values past it are appended
p Array.new(10**8) { |i| break i if i == 3; i * 2 }
p catch(:x) { Array.new(10**8) { |i| throw :x, -i if i == 4; i.to_f } }
p Array.new(70_000) { |i| i * 3 }.then { |a| [a.size, a[65_535], a[65_536], a.last] }
p Array.new(70_000) { |i| "s#{i}" }.then { |a| [a.size, a[65_536]] }
p [65_535, 65_536, 65_537, 131_073].map { |n| a = Array.new(n) { |i| i.to_f / 2 }; [a.size, a[n - 1], a.sum] }
p [65_536, 131_073].map { |n| a = Array.new(n) { |i| [i] }; [a.size, a[n - 1], a[65_535]] }
p [65_537].map { |n| a = Array.new(n) { |i| i.even? ? i : "o#{i}" }; [a.size, a[65_536], a[65_535]] }
p Array.new(5) { |i| break :early if i == 3; i }
p catch(:y) { Array.new(200_000) { |i| throw :y, i if i == 150_000; i } }
