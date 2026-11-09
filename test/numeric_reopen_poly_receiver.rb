# A Numeric reopening's method, called on a value that holds an Integer or a
# Float at run time, runs for both; an Integer reopening's own method of the
# name still runs for an Integer.
class Numeric
  def twice = self * 2
  def plus(n) = self + n
  def kind = :numeric
end

class Integer
  def kind = :integer
  def other = 0
end

xs = [1, 2.5, "s"]
p xs[0].twice
p xs[1].twice
p xs[0].plus(10)
p xs[1].plus(10)
p xs[0].kind
p xs[1].kind
begin
  xs[2].twice
rescue NoMethodError => e
  p e.class
end
p [3, 1.5].map { |v| v.twice }
