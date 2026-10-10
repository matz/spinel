# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# -2**63 held by an instance variable seeded 0, one seeded nil (nullable), a
# class variable, and read back through attr_reader, interpolation, nil? and
# truthiness.
class Box
  @@last = 0
  @@opt = nil
  attr_reader :v, :o
  def initialize
    @v = 0
    @o = nil
  end
  def set(x)
    @v = x
    @o = x
    @@last = x
    @@opt = x
  end
  def clear = @o = nil
  def show
    puts "v=#{@v} o=#{@o} last=#{@@last} opt=#{@@opt}"
    p [@v.nil?, @o.nil?, @@last.nil?, @@opt.nil?]
    p [@v ? 1 : 0, @o ? 1 : 0, @@last ? 1 : 0, @@opt ? 1 : 0]
  end
  def self.last = @@last
  def self.opt = @@opt
end
n = ARGV.size
m = -9223372036854775807 - (n + 1)
b = Box.new
b.show
b.set(m)
b.show
p b.v, b.o, Box.last, Box.opt
p b.v == m, b.o == m
b.clear
b.show
p b.o
