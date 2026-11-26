# a class body's loop over literal names where the program aliases each in
# Array: the alias runs, and the block does not
class Array
  alias each clear
end

class K
  [:x].each { |name| attr_accessor name }
end

p K.method_defined?(:x)
