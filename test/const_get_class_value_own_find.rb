# A class a method hands out from Object.const_get is a boxed value; its
# call of a name Enumerable has (find) is the class's own class
# method, as CRuby answers it. The call was rewritten to the builtin's
# Enumerable copy: refused ("unsupported puts argument"), or a NoMethodError
# for the class's #each at run time.
class Base
  def self.find(id) = "found #{id}"
end
class A < Base; end
module Admin
  class Post < Base; end
end

def pick(n)
  klass = Object.const_get(n)
  klass.is_a?(Class) && klass < Base ? klass : nil
end
def lookup(n) = Object.const_get(n)

puts pick("A").find(3)
parent = pick("Admin::Post")
puts parent.find(4)
puts lookup("A").find(5)
p pick("Admin")
# an Array beside it keeps the builtin
vals = [[1, 2, 3], lookup("A")]
p vals[0].find { |x| x > 1 }
p vals[1].find(6)
