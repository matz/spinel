# A bare call on main meets, in CRuby's lookup order, main's singleton class and its extends, Object's prepends, Object's
# own methods (the top-level defs), Object's includes, then Kernel; and each of them only from the statement that runs it.
# A bare `puts` before an include, an extend or an Object prepend of a module with a `puts` is Kernel's; after it, the
# module's, with main's extend ahead of an Object include, and a top-level def after an include ahead of it from there on.
puts "a"
module L
  def puts(*a) = $stdout.write("L:#{a.join}\n")
end
include L
puts "b"
def puts(*a) = $stdout.write("O:#{a.join}\n")
puts "c"
module E
  def print(*a) = $stdout.write("E:#{a.join}\n")
end
print "d\n"
extend E
print "d"

module L2
  def shout = "L2"
end
module E2
  def shout = "E2"
end
include L2
p shout
extend E2
p shout
module L3
  def shout = "L3"
end
include L3
p shout

module P
  def whisper = "P"
end
class Object
  def whisper = "O"
end
p whisper
Object.prepend(P)
p whisper
