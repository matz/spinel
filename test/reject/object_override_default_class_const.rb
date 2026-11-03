# A builtin class's override of an Object method whose default reads a
# constant the class defines: in CRuby the default reads Hash::D (3).
# Spinel moves the override into Object's method, where D would be
# Object's (7), so it refuses the program.
D = 7
class Object
  def o(x = D) = [:obj, x]
end
class Hash
  D = 3
  def o(x = D) = [:hash, x]
end
p({}.o)
