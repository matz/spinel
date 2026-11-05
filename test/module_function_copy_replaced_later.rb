# A module_function copy is a version of the module's singleton method, and a
# call in a copy's body meets the version live when it runs: the copy the
# bare word makes and the copy a name makes (`module_function :a, :b`), in
# the program's file and in a required one, and an alias in `class << self`
# of a copied name. The program replaces the singleton's method after the
# first call, and the copies' body calls follow.
module P
  module_function
  def a(x) = x + 1
  def b(x) = a(x) * 2
end
puts P.b(3)
def P.a(x) = 100
puts P.b(3)

module Q
  def a(x) = x + 1
  def b(x) = a(x) * 2
  module_function :a, :b
end
puts Q.b(3)
def Q.a(x) = 100
puts Q.b(3)

require_relative "module_function_copy_replaced_later/u"
puts U.b(3)
puts V.b(3)
def U.a(x) = 100
def V.a(x) = 200
puts U.b(3)
puts V.b(3)

module R
  def split_it(s) = s.split(" ")
  module_function :split_it
  class << self
    alias split split_it
  end
end
def call_split(s) = R.split(s)
p call_split("a b")
def R.split(s) = ["mine"]
p call_split("a b")

# the instance side is a method of its own, and the copy is a snapshot
module S
  def a = 1
  def b = a + 10
  module_function :a, :b
  def a = 2
end
puts S.b
class UsesS
  include S
  def go = b
end
puts UsesS.new.go
