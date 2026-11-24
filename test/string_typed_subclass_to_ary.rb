# A value typed String that is an instance of a String subclass with a
# to_ary of its own is taken apart by that to_ary in a multiple assignment
# after the def, also where the program's String has none: inherited
# through a subclass, overridden below it, private or protected, answering
# nil (the instance is kept), in a nested target, two in one statement, and
# over a value that may be nil. Before the def the instance is kept.
class String
  def same = self
end
class S < String
  private def to_ary = [:s, 1]
end
class T < S; end
class U < T
  def to_ary = [:u, 2]
end
class N < String
  def to_ary = nil
end
class P < String
  protected def to_ary = [:prot, 3]
end
class L < String; end
a, b = L.new("l").same
p [a, b]
class L
  def to_ary = [:l, 4]
end
a, b = L.new("m").same
p [a, b]
a, b = S.new("1").same
p [a, b]
a, b = T.new("2").same
p [a, b]
a, b = U.new("3").same
p [a, b]
a, b = (+"4").same
p [a, b]
a, b = N.new("n").same
p [a, b, a.class]
a, b = P.new("p").same
p [a, b]
(a, b), c = U.new("x").same, 9
p [a, b, c]
x = (a, b = U.new("y").same); y = (c, d = (+"z").same)
p [a, b, c, d, x, y]
def pick(n) = n ? nil : U.new("w").same
a, b = pick(true)
p [a, b]
a, b = pick(false)
p [a, b]
