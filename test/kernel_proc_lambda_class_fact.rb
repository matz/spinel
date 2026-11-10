# A Proc made by `Kernel.proc { }` or `Kernel.lambda { }` is the same value
# a bare `proc { }` makes. The class facts gave the bare forms the Proc set
# and an explicit Kernel receiver none, so a boxed `[]` on a value that is
# either such a Proc or an object of a class defining `[]` was typed from
# the class's method alone: the Proc's Integer was read as a String and
# the program crashed. Each spelling of the receiver is one here.
class Table
  def [] = "t"
end
p [Kernel.lambda { 1 }, Table.new][0][]
p [Kernel.proc { 2 }, Table.new][0][]
p [Kernel::lambda { 3 }, Table.new][0][]
p [Kernel::proc { 4 }, Table.new][0][]
p [::Kernel.proc { 5 }, Table.new][0][]
p [Kernel.lambda { 6 }, Table.new][1][]
