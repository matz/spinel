# An operator method called with a splat, `a.==(*rest)` or
# `public_send(*args)` naming one, takes the splat's one element as its
# operand. The array itself was the operand: == answered false and +
# raised TypeError. activesupport's Object#try is `public_send(*args)`.
def eq(a, *rest) = a.==(*rest)
p eq(1, 1)
p eq("x", "y")
def add(a, *rest) = a.+(*rest)
p add(3, 4)
p add("a", "b")
def lt(a, *rest) = a.<(*rest)
p lt(2, 3)
class Object
  def try1(*args) = public_send(*args)
end
p 1.try1(:==, 1)
p 3.try1(:+, 4)
begin
  add(1)
rescue ArgumentError => e
  p e.message
end
p 2.try1(:<=>, 3)
p [1].try1(:<<, 2)
# with a block beside the splat, as Object#try forwards one
def eqb(a, *rest, &blk) = a.==(*rest, &blk)
p eqb(1, 1)
def addb(a, *rest, &blk) = a.+(*rest, &blk)
p addb(3, 4)
class Foo
  def try4(*args, &block) = 1.public_send(*args, &block)
end
p Foo.new.try4(:==, 1)
p Foo.new.try4(:+, 2)
