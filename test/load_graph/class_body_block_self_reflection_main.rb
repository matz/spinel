# A bare name, to_s or inspect in a block of a class body runs on the class,
# whose self the block keeps, whether the block runs at once or is kept (a
# Hash's default block); a block whose call runs it with another self
# (instance_eval, a class builder) keeps its own. Only these answers, which
# cannot change during the load, are given the class in a block: ancestors,
# include?, is_a? and the method-table readers answer from the point the block
# runs at, so they are not. A `&expr` argument is not a block: the reflection
# in it is on the class, as it is in the class body itself.
class H
  def baz = "baz"
  [1].each { p name, to_s, inspect }
  T = Hash.new { |_, k| [k, name, to_s] }
  p [1].map(&(is_a?(Class) ? :to_s : :inspect))
end
p H::T[3]
class J
  o = Object.new
  o.instance_eval { p respond_to?(:one) }
  p Class.new { def two = 2 }.name
end
S = Struct.new(:a) do
  X = [1].map(&(members.size == 1 ? :succ : :pred))
end
p X
