# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# A nullable Integer ivar that was never assigned reads nil, defined?(@x) and
# instance_variable_defined? say so, and once assigned -2**63 it holds the
# number.
class C
  def initialize(f)
    if f
      @x = 5
    end
  end
  def get = @x
  def has = defined?(@x)
  def ivd = instance_variable_defined?(:@x)
  def set(v) = @x = v
  def show
    p [@x, @x.nil?, @x ? 1 : 0, defined?(@x), "#{@x}"]
  end
end
n = ARGV.size
m = -9223372036854775807 - (n + 1)
c = C.new(false)
p c.get, c.has, c.ivd
p c.get.nil?
c.show
c.set(m)
p c.get, c.has, c.ivd
p c.get.nil?
c.show
d = C.new(true)
p d.get, d.has, d.ivd
d.set(m)
p d.get
p d.get == m
e = C.new(n > 5)
p e.get
p e.has
e.set(m)
p e.get + 1
