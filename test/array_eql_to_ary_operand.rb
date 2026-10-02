# Array#eql? does not defer to an operand that answers to_ary, though == does
class Trail
  def initialize(items) = @items = items
  def ==(other) = @items == other
  def eql?(other) = true
  def to_ary = @items
end

def same?(a, b) = a == b
def eql_pair?(a, b) = a.eql?(b)

t = Trail.new([1, 2])
p same?([1, 2], t)
p eql_pair?([1, 2], t)
p same?([1, 2], [1, 2])
p eql_pair?([1, 2], [1, 2])
p eql_pair?([1, 2], [1, 2.0])
p eql_pair?([1, 2], 1)
p eql_pair?([[1, 2]], [t])
p same?([[1, 2]], [t])
