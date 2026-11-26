# a class body's loop over literal names where the program reopens
# Array#each: the program's each runs, and the block does not
class Array
  def each = puts("custom")
end

class K
  [:x].each { |name| attr_accessor name }
end

p K.method_defined?(:x)
