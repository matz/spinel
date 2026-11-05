# A def chosen by a run-time condition is a dispatcher and its arms
# (`a#__cond1`, ...). A module_function copy of the dispatcher calls the
# arms on the module's side, so the module has them once one of them is
# sure to have run where the copy is made: the first def here is not under
# the condition. By name and with the bare word, with arguments, under an
# alias, and after the program replaces the copy.
module M
  def a = 1
  def a = 2 if ARGV.size > 5
  module_function :a
end
p M.a
def M.a = 9
p M.a

module N
  def a = 1
  def a = 2 if ARGV.size > 5
  def b = a + 10
  module_function :a, :b
end
p N.b

module O
  module_function
  def a = 1
  def a = 2 if ARGV.size > 5
  def b = a + 10
end
p O.b
def O.a = 9
p O.b

module P
  def a(x) = x + 1
  def a(x) = x + 2 if ARGV.size > 5
  def b(x) = a(x) * 10
  module_function :a, :b
end
p P.b(1)
def P.a(x) = x + 100
p P.b(1)

module Q
  def a = 1
  def a = 2 if ARGV.size > 5
  alias b a
  module_function :b
end
p Q.b

module U
  module_function
  def a = 1
  def a = 2 if ARGV.size > 5
end
class C
  include U
  def t = a
end
p U.a
p C.new.t
