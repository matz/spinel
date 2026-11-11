# A block's `b.call` whose value goes into a boxed slot (an Array element, a
# Hash value, a String interpolation) is spliced where it stands, and the
# block's tail is boxed to match. Only an object, a String call or a String
# range arm was: a block ending in an if, unless, case or rescue modifier
# over Strings (`{ c ? s : t }`) handed its bytes to the boxed slot, so the
# C did not compile, where `yield` boxed the same tail. The tail is now
# boxed whenever it is not a boxed value already, and the box carries the
# String's handle, so a change through the slot reaches the variable.
def fw(&b) = [0, b.call]
def fh(&b) = {a: b.call}
def fp(&b) = [0, b.call(1)]
def two(&b) = [b.call, b.call]
def fy = [0, yield]

s = +"s"
p fw { 1 > 0 ? s : +"t" }
p fw { 1 > 0 ? "x" : "y" }
p fh { 1 > 0 ? s : +"t" }
p fw { case s.size when 1 then s else +"t" end }
p fw { unless s.empty? then s else +"t" end }
n = 3
p fw { if n == 1 then +"one" elsif n == 3 then s else +"t" end }
p fp { |x| x > 0 ? s : +"t" }
p fw { 1 > 0 ? s : nil }
p fw { 1 < 0 ? s : nil }
p fw { begin; raise "x" if s.size > 5; s; rescue; +"r"; end }
def risky(v) = (raise("x") if v.size > 0)
p fw { risky(s) rescue +"fb" }
t = +"abc"
p fw { 1 > 0 ? t[0] : t }
p fw { t.size > 5 ? nil : t[1, 2] }

# the box holds the String itself: a change through it is seen by s
a = fw { if s.size > 0 then s else +"t" end }
a[1] << "!"
p a, s
h = fh { 1 > 0 ? s : +"t" }
h[:a] << "?"
p h, s
r = two { 1 > 0 ? s : +"t" }
r[0] << "1"
p r, s
a2 = fy { if s.size > 0 then s else +"t" end }
a2[1] << "#"
p a2, s

# a mutator that answers the String, and an ivar
buf = +"b"
p fw { 1 > 0 ? buf << "x" : buf }
p buf
class K
  def initialize; @s = +"iv"; end
  def fw(&b) = [0, b.call]
  def go = fw { @s.size > 0 ? @s : +"t" }
end
k = K.new.go
k[1] << "!"
p k

# tails that were boxed or did not need it
p fw { 1 > 0 ? 7 : 8 }
p fw { 1 > 0 ? :a : :b }
p fw { 1 > 0 ? s : 5 }
p fw { 1 < 0 ? s : 5 }
p fw { 1 > 0 ? true : "s" }
x = 1.5
p fw { 1 > 0 ? x : 2.5 }
p fw { case 1 when 1 then :a else :b end }
p fw { 1 > 0 ? nil : nil }
i = 0
p fw { while i < 2; i += 1; end }
class O; def n = 1; end
class P; def n = 2; end
p fw { 1 > 0 ? O.new : P.new }[1].n
p fw { case s.size when 1 then 7 else "no" end }
p fw { case s.size when 1 then s when 2 then 7 else :x end }
