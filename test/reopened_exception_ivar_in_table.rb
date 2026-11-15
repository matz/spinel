# An instance variable in a method a program adds to a builtin exception
# class lives in the table keyed by the object, as an Array's does: the
# builtin exception has no layout of the program's. A program class under
# it reads and writes the same variable there, and keeps its own in its
# layout.
class RuntimeError
  def initialize(m = nil)
    super(m)
    @own = 1
  end
  attr_reader :own
  def mark = (@own = 5)
end
r = RuntimeError.new("x")
p r.message, r.own
r.mark
p r.own

class NoMethodError
  def tag! = (@tag = :seen)
  def tag = @tag
  def tagged? = defined?(@tag) ? true : false
end
n = NoMethodError.new("n")
p n.tag, n.tagged?
n.tag!
p n.tag, n.tagged?

class Sub < RuntimeError
  def initialize(m)
    super
    @mine = 2
  end
  def both = [@own, @mine]
  def bump = (@own += 10)
end
s = Sub.new("s")
p s.both, s.own
s.mark
p s.both
s.bump
p s.both, s.own, s.message

class Deep < Sub
  def own_twice = @own * 2
end
d = Deep.new("d")
p d.own_twice, d.both
