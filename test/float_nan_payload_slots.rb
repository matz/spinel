# The NaN with payload 0x7FF8000000000001 through a Float parameter, a
# return, a nullable parameter and return, an ivar seeded 0.0, an ivar seeded
# nil, a typed Float array element, a Float Hash value, a lazily filled Array
# and a nullable local: NaN everywhere, nil nowhere.
def nan_of(n) = ("\x01\x00\x00\x00\x00\x00\xF8\x7F" * (n + 1)).unpack1("d")
def plain(v) = v
def opt(v) = v
def ret_opt(n)
  return nil if n > 100
  nan_of(n)
end
class Box
  attr_reader :f, :o
  def initialize
    @f = 0.0
    @o = nil
  end
  def set(v)
    @f = v
    @o = v
  end
  def show
    puts "f=#{@f} o=#{@o}"
    p [@f.nil?, @o.nil?, @f.nan?, @o&.nan?]
  end
end
n = ARGV.size
x = nan_of(n)
p plain(x)
p plain(x).nan?
p opt(nil)
p opt(x)
p opt(x).nil?
p opt(x).nan?
p ret_opt(1000)
p ret_opt(n)
p ret_opt(n).nil?
r = ret_opt(n)
p(r ? 1 : 2)
b = Box.new
b.show
b.set(x)
b.show
p b.f.nan?, b.o.nan?, b.o.nil?
a = Array.new(2, 0.0)
a[1] = x
p a
p a[1].nan?
p a[1].nil?
p a.map(&:nan?)
p a.count(&:nan?)
p a.sum.nan?
h = { 1 => 0.0 }
h[2] = x
p h[2]
p h[2].nan?
p h[2].nil?
p h[3].nil?
p h.values.map(&:nan?)
lz = Array.new(3)
lz[1] = x
p lz
p lz[1].nan?
p lz[0].nil?
p lz.compact.size
s = nil
s = x if n == 0
p s
p s.nil?
p s.nan?
p(s ? "set" : "unset")
t = 1.5
t = nil if n > 5
t = x
p t
p t.nil?
[x, nil].each { |e| p e.nil? }
[x].each { |e| p e.nan?, e.nil? }
$g = x
p $g
p $g.nan?
