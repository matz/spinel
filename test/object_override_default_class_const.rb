# A builtin class's override of an Object method whose default reads a
# constant the class defines: in CRuby the default reads Proc::D (3).
# Spinel moves the override into Object's method; the read is the path
# Proc::D from before the move, so it does not become Object's D (7).
D = 7
class Object
  def o(x = D) = [:obj, x]
end
class Proc
  D = 3
  def o(x = D) = [:proc, x]
end
p(proc { 1 }.o)
p 5.o
