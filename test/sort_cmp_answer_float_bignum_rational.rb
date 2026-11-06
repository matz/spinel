# spinel: int64
# A <=> answer that is a Float, a Bignum or a Rational orders sort by its sign.
class F; attr_reader :n; def initialize(n) = @n = n; def <=>(o) = (n - o.n) * 0.25; end
class G; attr_reader :n; def initialize(n) = @n = n; def <=>(o) = (n - o.n) * (2**70); end
class R; attr_reader :n; def initialize(n) = @n = n; def <=>(o) = Rational(n - o.n, 3); end
p({F.new(2) => 1, F.new(1) => 2}.sort.map(&:last))
p [[F.new(2), 1], [F.new(1), 2]].sort.map(&:last)
p [F.new(3), F.new(1), F.new(2)].sort.map(&:n)
p [F.new(3), F.new(1)].minmax.map(&:n)
p({G.new(2) => 1, G.new(1) => 2}.sort.map(&:last))
p [[G.new(2), 1], [G.new(1), 2]].sort.map(&:last)
p [G.new(3), G.new(1), G.new(2)].sort.map(&:n)
p [G.new(3), G.new(1)].minmax.map(&:n)
p({R.new(2) => 1, R.new(1) => 2}.sort.map(&:last))
p [[R.new(2), 1], [R.new(1), 2]].sort.map(&:last)
p [R.new(3), R.new(1), R.new(2)].sort.map(&:n)
p [R.new(3), R.new(1)].minmax.map(&:n)
x = [F.new(2), 1].first
p x <=> F.new(1)
y = [R.new(2), 1].first
p y <=> R.new(1)
z = [G.new(2), 1].first
p z <=> G.new(1)
class Q; attr_reader :n; def initialize(n) = @n = n; def <=>(o) = Rational(o.n - n, -3); end
p [Q.new(3), Q.new(1), Q.new(2)].sort.map(&:n)
p({Q.new(2) => 1, Q.new(1) => 2}.sort.map(&:last))
class P; attr_reader :n; def initialize(n) = @n = n; def <=>(o) = n > 50 ? nil : (n > 40 ? Rational(n - o.n, 3) : (n - o.n) * 0.5); end
p [P.new(3), P.new(1), P.new(2)].sort.map(&:n)
p [P.new(43), P.new(41), P.new(42)].sort.map(&:n)
p({P.new(2) => 1, P.new(1) => 2}.sort.map(&:last))
p({P.new(42) => 1, P.new(41) => 2}.sort.map(&:last))
p(([P.new(51), P.new(1)].sort rescue $!.class))
