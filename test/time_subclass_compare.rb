# A Time subclass instance is its Time: Time's comparisons take it as an
# operand, on either side, and a subclass with its own <=> answers them
class Stamp < Time; end
class Sub2 < Stamp; end
class Own < Time
  def <=>(o) = 0
end

a = Stamp.at(0).utc
d = a.dup
p a == d
p a.eql?(d)
p a != d
p a == Time.at(0).utc
p Time.at(0).utc == a
p a == Stamp.at(0).utc
p a <=> d
p a < Stamp.at(60)
p a >= d, a <= d, a > d
p Time.at(0) < Stamp.at(60)
p Time.at(0) <=> Stamp.at(60)
p Time.at(0).eql?(Stamp.at(0))
p a.between?(Stamp.at(0), Stamp.at(9))
p Stamp.at(60) - Stamp.at(0)
p Time.at(60) - Stamp.at(0)
p [Stamp.at(3), Stamp.at(1)].sort.map(&:to_i)
p [Stamp.at(3), Stamp.at(1)].max.to_i
p({ a => 1 }[d])
p a.equal?(d)
p Stamp.at(0) == 0

# a deeper subclass, a parameter, a value that may be nil
b = Sub2.at(0)
p a == b, b == a, a < Sub2.at(5), b <=> a
def same(x, y) = x == y
p same(a, Stamp.at(0))
m = ARGV.size > 3 ? Stamp.at(0) : nil
p Time.at(0) == m
p((Time.at(0) < m rescue $!.class))
n = ARGV.size < 3 ? Stamp.at(0) : nil
p Time.at(0) == n
p Time.at(0).eql?(n)

# the subclass's own <=> answers Comparable's operators
o = Own.at(0)
p o == Time.at(99)
p o < Time.at(99)
p Time.at(99) == o
p o <=> Time.at(5)
