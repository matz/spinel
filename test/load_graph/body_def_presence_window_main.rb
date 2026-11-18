# A respond_to?, defined? or method_defined? of a method a class method's
# body defines answers as the call it guards: false until the body has run.
class K
  def self.setup
    def m = :k
  end
end
class J
  def m = :j
end
class S < K; end
class L
  def self.a
    def m = :a
  end
  def self.b
    def m = :b
  end
end
class Q
  def self.setup
    def m = :q
  end
  def try = [respond_to?(:m) ? m : :none, defined?(m) ? m : :none, respond_to?("m")]
end
class R < Q; end

def go(xs) = xs.map { |x| x.respond_to?(:m) ? x.m : :none }
def use(k) = k.respond_to?(:m) ? k.m : :none
xs = [K.new, J.new, S.new, nil, 3]
k = K.new
l = L.new
seen = k.respond_to?(:m)
r = []
r << k.m if k.respond_to?(:m)
p go(xs), use(k), seen
p xs.map { |x| defined?(x.m) ? :yes : :no }
p xs.first(3).map { |x| x.class.method_defined?(:m) }
p K.method_defined?(:m), S.method_defined?(:m, false), K.public_method_defined?(:m)
p(l.respond_to?(:m) ? l.m : :none)
qs = [Q.new, R.new]
p qs.map(&:try)
K.setup
r << k.m if k.respond_to?(:m)
p go(xs), use(k), seen, r
p xs.map { |x| defined?(x.m) ? :yes : :no }
p xs.first(3).map { |x| x.class.method_defined?(:m) ? x.m : :none }
p K.method_defined?(:m), S.method_defined?(:m, false), K.public_method_defined?(:m)
L.a
p(l.respond_to?(:m) ? l.m : :none)
L.b
p(l.respond_to?(:m) ? l.m : :none)
Q.setup
p qs.map(&:try)
# A respond_to? in a method of a name whose versions differ in visibility
# reads the cells of the public ones, the form it asks.
class PB
  private def pm = :b
end
class PK < PB; end
pk = PK.new
def pchk(k) = [k.respond_to?(:pm), k.respond_to?(:pm, true)]
p pchk(pk)
class PK
  def pm = :k
end
p pchk(pk)
