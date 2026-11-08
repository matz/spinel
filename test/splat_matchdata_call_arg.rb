# A MatchData splat spreads its to_a (the whole match, then each group)
# into a method, a constructor, a proc, a lambda, a yield and a Method
# call. A nil MatchData (no match) spreads nothing.
class P
  def initialize(*a) = (@a = a)
  def show = p(@a)
end
class Q < P
  def initialize(*a) = super(*a)
end
def f(*a) = a
def g(a, b, c, d) = [a, b, c, d]
def h(a, *r) = [a, r]
def y1(r) = yield(*r)
m = /(a)(b)?(c)/.match("ac")
p f(*m)
p g(*m)
p h(*m)
p f(1, *m, 2)
P.new(*m).show
Q.new(*m).show
pr = proc { |*a| a }
p pr.call(*m)
l = ->(a, b, c, d) { [a, b, c, d] }
p l.call(*m)
p y1(m) { |*a| a }
p method(:h).call(*m)
p send(:h, *m)
n = /(z)/.match("ac")
p f(*n), h(0, *n), pr.call(*n)
puts(*m)
x = []
x.push(*m)
p x, [0, *m]
p format("%s-%s-%s-%s", *m)
