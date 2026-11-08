# A reopen of an Integer iterator with keyword or **rest parameters takes
# every call, also one with a block (`3.times { }`), as CRuby runs the
# program's method. The call with a block used to stay on the builtin loop.
class Integer
  def times(**kw) = kw
  def upto(x, **kw) = [x, kw]
  def downto(x, k: 5) = [x, k]
  def step(lim, by: 1, &b) = [lim, by, b ? b.call(1) : nil]
end
p 3.times(a: 1)
p 3.times { }
p 3.times
h = {z: 2}
p 3.times(**h)
p 1.upto(3) { }
p 1.upto(3, q: 1)
p 5.downto(1) { }
p 5.downto(1, k: 7)
p 1.step(9, by: 3) { |x| x * 2 }
p 1.step(9)
x = [3, 4].sample(1).first
p x.times(w: 1)
def f(n) = n.times(a: n)
p f(2)
