# The 32-bit twin of the -2**63 tests: -2**31, the INT32_MIN of a 32-bit
# target, reached by arithmetic that does not overflow, is a number in every
# Integer slot there too. No int64 marker: this runs on both widths.
n = ARGV.size
m = -2147483647 - (n + 1)
p m
puts "#{m}"
p m.nil?
p(m ? "truthy" : "falsy")
p m.zero?
p m + 1
p m - n
p m * 1
p m & 1
p m >> 31
p m == m
p m < 0
p [n, m].min
p [-2147483647, -(n + 1)].sum
p [m, nil].compact
p [m].pack("l<").unpack1("l<")
p ~0x7fffffff
p ~(0x7fffffff - n)
x = 5
x = nil if m == 0
x = m
p x
p x.nil?
y = nil
y = m if n == 0
p y
class Box
  def initialize = @v = nil
  def set(v) = @v = v
  attr_reader :v
end
b = Box.new
b.set(m)
p b.v
p b.v.nil?
def f(v) = v
f(nil)
p f(m)
a = Array.new(2, 0)
a[1] = m
p a
p a[1].nil?
p a.min
lz = Array.new(4)
lz[3] = m
p lz
p lz[3].nil?
p lz.compact
h = { 1 => 0 }
h[1] = m
p h[1]
p h[1].nil?
p h[2].nil?
p h.fetch(2, m)
g = Hash.new(m)
p g[0]
r = (m..m + 1)
p r.to_a
p r.begin.nil?
case m
when nil then puts "nil"
when ..-1 then puts "negative"
end
