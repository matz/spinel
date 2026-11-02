# A class method the program defines under a name the class answers itself
# (name, to_s, inspect) leaves a call that runs before the def to the
# builtin, as CRuby does: from a method, a lambda or a block, at a load
# point, on a subclass and on a module. After the def the program's method
# answers. (One def per name: a name the program defines twice keeps the
# table's answer.)

class C; end
class D < C; end
def from_method = C.name
def name_of(k) = k.name
class C
  def self.label = "<#{name}>"
end
from_lambda = -> { C.name }
def calls = [from_method, name_of(D), name_of(C), C.label, C.then { |k| k.name }, [C].map(&:name)]
p calls, from_lambda.(), C.name
def C.name = "x"
p calls, from_lambda.(), C.name

module M; end
def m_text = M.to_s
p m_text
module M
  def self.to_s = "m"
end
p m_text

class E; end
p E.inspect
e_lambda = -> { E.inspect }
p e_lambda.()
class E
  def self.inspect = "e"
end
p e_lambda.(), E.inspect
