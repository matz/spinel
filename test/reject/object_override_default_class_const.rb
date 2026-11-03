# A builtin class's override of an Object method whose default reads a
# constant the class may define (the write is under a condition): in
# CRuby the default reads Proc::D (3) once the write ran. Spinel moves
# the override into Object's method, where D would be Object's (7), so
# it refuses the program.
D = 7
class Object
  def o(x = D) = [:obj, x]
end
class Proc
  D = 3 if ARGV.empty?
  def o(x = D) = [:proc, x]
end
p(proc { 1 }.o)
