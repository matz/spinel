# A bare module_function def is the instance method's version and the module
# singleton's copy at once, and a call that meets either side reads the cell of
# its own history. The second reopening makes two versions on both sides: U.b
# is called before and after it, and the top-level include makes `b` a call
# that meets the instance side too.
module U
  module_function
  def a = 1
  def b = a
end
puts U.b
module U
  module_function
  def a = 2
end
puts U.b
include U
puts b
