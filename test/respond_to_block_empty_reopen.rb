# A reopening of a builtin class that holds nothing of a method's name leaves
# a literal's call of that method the builtin's: its block runs at once, so a
# respond_to? in it answers before a later def. An empty `class Object; end`,
# a bare `private`, an `include` of an empty module, an attr_reader of another
# name, an Object def of another name, and a reopening after the call all hold
# nothing of `each`, `map` or `yield_self`. A reopening that does hold the
# name, or mixes in a module that does, keeps the block for a later run: the
# question is answered then, whether the reopening is placed at once or is
# not (a def in a proc body).

class S1; end
class S2; end
class S3; end
class S4; end
class S5; end
class K1; end
class K2; end
class K3; end
class K4; end
class K5; end
class K6; end

module EmptyM; end
module KeepEachWithIndex
  def each_with_index(&b) = ($k3 = b; self)
end
module KeepEach
  def each(&b) = ($k4 = b; self)
end
module KeepThen
  def then(&b) = ($k5 = b; self)
end

class Object
end

class Object
  include EmptyM
  attr_reader :zz_unused
  private
end

class Object
  def other_name = 1
end

# a public top-level def is an Object method; this one runs its block at once
public def yield_self(&b) = (yield self; self)

[1].each { p S1.respond_to?(:z) }
(1..2).each { p S2.respond_to?(:z) }
[1].map { p S3.respond_to?(:z) }
:a.yield_self { p S4.respond_to?(:z) }
def S1.z = 1
def S2.z = 1
def S3.z = 1
def S4.z = 1
p [S1, S2, S3, S4].map { |s| s.respond_to?(:z) }

# a reopening after the call does not matter to it either
[2].each { p S5.respond_to?(:z) }
class Array
  def each_later = 1
end
def S5.z = 1
p S5.respond_to?(:z)

# a reopening that keeps the block: asked when the kept block runs
class Object
  def tap(&b) = ($k1 = b; self)
end
1.tap { p K1.respond_to?(:z) }
class Integer
  def times(&b) = ($k2 = b; self)
end
2.times { p K2.respond_to?(:z) }
class Array
  include KeepEachWithIndex
end
[3].each_with_index { p K3.respond_to?(:z) }
class Hash
  prepend KeepEach
end
{ k: 4 }.each { p K4.respond_to?(:z) }
# a top-level include is an include into Object
include KeepThen
5.then { p K5.respond_to?(:z) }
# a def in a proc body that is called is held but not placed
pr = proc do
  class Hash
    def each_key(&b) = ($k6 = b; self)
  end
end
pr.call
{ k: 6 }.each_key { p K6.respond_to?(:z) }
def K1.z = 1
def K2.z = 1
def K3.z = 1
def K4.z = 1
def K5.z = 1
def K6.z = 1
$k1&.call
$k2&.call
$k3&.call
$k4&.call
$k5&.call
$k6&.call
