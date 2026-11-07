# A builtin class's override of an Object method whose default reads a
# constant the class may define (the write is under a condition): in
# CRuby the default reads Proc::D (3) once the write ran, and Object's D
# (7) where it did not. The read asks which at run time.
D = 7
class Object
  def o(x = D) = [:obj, x]
end
class Proc
  D = 3 if ARGV.empty?
  def o(x = D) = [:proc, x]
end
p(proc { 1 }.o)
