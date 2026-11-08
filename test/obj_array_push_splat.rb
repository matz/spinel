# push / append with a `*splat` onto an Array of program objects: each
# element of the splat is pushed, of the same class or another one. The
# C did not compile: the object-array push took the splat as one element.
class A; def foo = "A"; end
class B; def foo = "B"; end
a = [A.new]; b = [B.new]; a.push(*b); a.each { |x| puts x.foo }
s = [A.new]; t = [A.new, A.new]; s.push(*t); p s.size
u = [A.new]; u.append(B.new, *[B.new, A.new], A.new); p u.map(&:foo)
w = [A.new]; p w.push(*[]).size
def f(*r) = (q = [A.new]; q.push(*r); q.map(&:foo))
p f(B.new, A.new)
class C
  def initialize = @a = [A.new]
  def add(*r) = @a.push(*r)
  def all = @a.map(&:foo)
end
c = C.new; c.add(B.new, A.new); p c.all
