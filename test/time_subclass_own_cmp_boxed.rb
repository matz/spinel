# A Time or Array subclass of the program's with its own <=> or ==: sort,
# min/max, a boxed receiver's <, <=> and ==, and a typed != call that
# method, as CRuby does.

class Rev < Time
  def <=>(o) = o.to_i <=> to_i
end
a = Rev.at(0)
b = Rev.at(60)
p a < b, a > b, a == b, a <=> b
p [a, b].sort.map(&:to_i)
p [a, b].max.to_i, [a, b].min.to_i
p a.between?(b, b), a.clamp(b, b).to_i
p Time.at(0) < b, Time.at(0) <=> b
x = [a, 5].first
p x < b, x <=> b
t = ARGV.empty? ? b : nil
p Time.at(30) < t
p Time.at(30) <=> t
p Time.at(30) == t
p b == Time.at(60)
p Time.at(60) == b
p b.eql?(Time.at(60))
p [b, a].sort { |u, v| u <=> v }.map(&:to_i)
p [a, b].minmax.map(&:to_i)
p [a, b].sort_by(&:to_i).map(&:to_i)

class Stamp < Time
  def ==(o) = :mine
end
a = Stamp.at(0)
b = Stamp.at(60)
p a == b
p a != b
x = [a, 5].first
p x == b
def eqq(u, v) = u == v
p eqq(a, b)

class Rows < Array
  def ==(o) = :rows_eq
  def <=>(o) = o.size <=> size
end
a = Rows.new([1, 2]); b = Rows.new([1])
x = [a, 5].first
p x == b
p x <=> b
p [a, b, Rows.new([1,2,3])].sort.map(&:size)
p [a, b].max.size

# an own <=> that gives nil: sort and max raise, as CRuby does
class NoOrder < Array
  def <=>(o) = nil
end
begin
  [NoOrder.new([1, 2]), NoOrder.new([3])].sort
rescue ArgumentError => e
  puts e.class
end
begin
  [NoOrder.new([1, 2]), NoOrder.new([3])].max
rescue ArgumentError => e
  puts e.class
end

# `!= nil` asks the subclass's own ==
def ne_nil(x) = x != nil
p ne_nil(Stamp.at(0)), ne_nil(Time.at(0))
st = Stamp.at(5)
p st != nil
