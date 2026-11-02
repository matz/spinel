# The class a singleton definition on an untraceable receiver is parked on is
# made by the compiler, under a name that comes from the program's own nodes:
# a class the program writes as Object__sg_untraced is its own class, and the
# singleton definition still defines its method on the one object.

class Object__sg_untraced
  def hi = 1
end

class Foo; end

def parked(flag)
  o = Foo.new
  o = Foo.new if flag
  def o.from_def = "def"
  o
end

p Object__sg_untraced.new.hi
p Object__sg_untraced.name
p Object__sg_untraced.new.class
puts((begin; from_def; rescue NameError; "NameError"; end))
puts respond_to?(:from_def, true)
puts Object.new.class
