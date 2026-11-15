# A class method defined twice, called between the two definitions, where a
# module mixed into Object later has a method of the same name: the mixin
# stands behind the class's own method on its singleton chain, so the call
# still meets the version live when it runs.
class U
  def self.a = 1
  def self.b = a
end
puts U.b
class U
  def self.a = 2
end
puts U.b
module V
  def a = 7
end
Object.include(V)
puts 1.a

# a module function's copy is the module's own method the same way
module F
  module_function
  def g = 1
  def h = g
end
puts F.h
module F
  module_function
  def g = 2
end
puts F.h
Object.include(F)
puts F.h

# and a reopening of Object that includes it
module P
  def self.q = :one
  def q = :inst
  def self.r = q
end
p P.r
module P
  def self.q = :two
end
p P.r
class Object
  include P
end
p P.r
