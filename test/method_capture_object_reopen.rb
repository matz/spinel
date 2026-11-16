# A Method captured on main of a name the program's `class Object` reopening defines is that method: Object's own entry
# is on main's chain, ahead of Kernel's.
class Object
  def shout(*a) = $stdout.write("O:#{a.join}\n")
  def puts(*a) = $stdout.write("P:#{a.join}\n")
end
m = method(:shout)
m.call("x")
p m.owner
pm = method(:puts)
pm.call("y")
