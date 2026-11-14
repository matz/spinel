# An alias in a subclass of a name the superclass defines under a run-time
# condition, the subclass's own def of that name after the alias and a later
# reopening of the superclass that defines it again: the alias still holds a
# def of the superclass (it raised NoMethodError when it took the subclass's
# later def). The reopening's def answers what the branch that ran answers,
# so the line reads the same whichever of the two the alias holds.
# The same in a module the subclass includes: the module's later plain def is a
# version of its own, which the alias, made before it, does not take, and the
# conditional group stays where it was (alias and alias_method, a reopen that
# defines the same body and another).
class B
  if ARGV.size > 0
    def foo = :b1
  else
    def foo = :b2
  end
end
class K < B
  alias bar foo
  def foo = :k
end
class B
  def foo = :b2
end
p K.new.bar
p K.new.foo
module M
  if ARGV.size > 0
    def foo = :m1
  else
    def foo = :m2
  end
end
class L
  include M
  alias bar foo
  def foo = :l
end
module M
  def foo = :m2
end
p L.new.bar, L.new.foo
module N
  if ARGV.size > 0
    def foo = :n1
  else
    def foo = :n2
  end
end
class O
  include N
  alias bar foo
  def foo = :o
end
module N
  def foo = :n3
end
p O.new.bar, O.new.foo
module R
  if ARGV.size > 0
    def foo = :r1
  else
    def foo = :r2
  end
end
class S
  include R
  alias_method :bar, :foo
  def foo = :s
end
module R
  def foo = :r2
end
p S.new.bar, S.new.foo
