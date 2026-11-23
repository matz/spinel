# Enumerable#flat_map asks `v.class.private_method_defined?(:to_ary)`, a
# method_defined? whose receiver only the run time knows, so the program
# keeps a table of the answers. The table held a row for every method name
# of the program; a row whose presence a prepend guard or a private mark
# decides read version cells that nothing else read, and the compiler
# stopped ("a presence test reads the version cell ..., which no reader
# kept"). Every caller names its method by a literal, so the table holds
# those names' rows alone.
def a = [1].any?
def restore(name) = send("#{name}=", 1)
module P
  def shadowed; end
  private :shadowed
end
class C
end
class C
  prepend P
end

module Splat
  private
  def to_ary = [:x, :y]
end
class Pair
  def initialize(v) = @v = v
end
class Pair
  prepend Splat
end
p [1, 2].flat_map { |i| [i, i] }
p [Pair.new(1)].flat_map { |o| o }.size
p C.private_method_defined?(:shadowed), C.new.respond_to?(:shadowed)
