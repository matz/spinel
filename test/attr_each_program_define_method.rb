# a class body's loop over literal names where the program gives Array an
# each with define_method: the program's each runs, and the block does not
class Array
  define_method(:each) { puts "custom" }
end

class K
  [:x].each { |name| attr_accessor name }
end

p K.method_defined?(:x)
