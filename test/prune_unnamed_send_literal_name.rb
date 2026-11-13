# A send whose name is a literal finds that method whatever its other
# arguments hold: the methods nothing names may still be left out.
module Helper
  def self.twice(v) = v * 2
end
class T
  def unused_a(xs) = xs.sum
  def last_one = :kept
end
v = 21
p Helper.send(:twice, v)
p T.new.last_one
