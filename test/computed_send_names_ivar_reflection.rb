# A send of a computed name the program may spell as a reflection
# (`:instance_variable_set`) writes the ivar it names: its arms are made
# after the class facts, which then take every ivar as written.
class A
  def hi = :a
end
class C
  def hi = :c
end
class Box
  def initialize = @v = C.new
  def v = @v
end
op = ARGV[0] ? :x : :instance_variable_set
b = Box.new
b.send(op, :@v, A.new)
p b.v.hi
rd = ARGV[0] ? :y : :instance_variable_get
p b.send(rd, :@v).hi
