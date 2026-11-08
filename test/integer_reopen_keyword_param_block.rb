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

# a call that misses a required keyword or passes an unknown one raises
# CRuby's ArgumentError
class Integer
  def gcd(x, k:) = [x, k]
  def lcm(x, k: 1) = [x, k]
end
begin; p 4.gcd(2); rescue ArgumentError => e; p e.message; end
p 4.gcd(2, k: 3)
begin; p 4.lcm(2, z: 1); rescue ArgumentError => e; p e.message; end
p 4.lcm(2, k: 5)
