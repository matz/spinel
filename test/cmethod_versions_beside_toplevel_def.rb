# A class method defined twice, called from another class method between the
# two definitions, beside a top-level def (or an Object method) of the same
# name: the call meets the version live when it runs. The top-level def sits
# at the end of every class's singleton chain, behind the class's own method,
# so it is no version the call can meet.
class U
  def self.a = 1
  def self.b = a
end
puts U.b
class U
  def self.a = 2
end
puts U.b
def a = 7
puts a

class W
  def self.c = 10
  def self.d = c
end
puts W.d
class W
  def self.c = 20
end
puts W.d
class Object
  def c = 70
end
puts 1.c

module M
  def self.e = :first
  def self.f = e
end
p M.f
module M
  def self.e = :second
end
p M.f
def e = :top
p e
