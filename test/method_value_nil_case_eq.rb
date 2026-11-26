# `===` on a Method value that is nil (`super_method` with no super) is
# nil's own `===`, true for a nil argument and false otherwise. A live
# Method's `===` calls it.
class C
  def hi(x) = x * 3
end
class D < C
  def hi(x) = super + 1
end
m = C.new.method(:hi).super_method
p(m === 2)
p(m === nil)
v = m === 2
p v
p((m === 2) ? :t : :f)
live = D.new.method(:hi).super_method
p(live === 2)
def pick(f) = f ? C.new.method(:hi) : nil
p(pick(false) === nil)
p(pick(true) === 2)
$n = 0
def tick = ($n += 1)
p(m === [tick])
p(live === tick)
p $n
class N
  def nilq(x) = x.nil?
end
nq = N.new.method(:nilq)
p nq.call(nil)
def maybe(f) = ($n += 1; f ? 7 : nil)
p(nq === maybe(false))
p(nq === maybe(true))
p $n
def nil_tick(a) = nil
p(nq === nil_tick([tick]))
p $n
