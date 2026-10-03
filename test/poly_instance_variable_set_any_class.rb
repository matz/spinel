# instance_variable_set on a boxed receiver creates the ivar on whichever
# object it is, as in CRuby, also when its class never writes that ivar:
# every class that can take one lays it out. The write to such a class was
# dropped and instance_variable_get answered nil. A Data instance is
# frozen, and the write raises FrozenError.

class K; end
class L; def initialize = (@a = "s"); end
class P; def initialize(v) = (@v = v); attr_reader :v; end

x = [K.new, 1][0]
p x.instance_variable_set(:@a, 5), x.instance_variable_get(:@a)

xs = [K.new, L.new]
xs.each { |o| o.instance_variable_set(:@a, 7) }
p xs.map { |o| o.instance_variable_get(:@a) }

y = [P.new(1), 2][0]
y.instance_variable_set(:@w, "w")
p y.instance_variable_get(:@w), y.v
y.instance_variable_set(:@v, 9)
p y.v

D = Data.define(:a)
z = [D.new(a: 1), 2][0]
p(begin; z.instance_variable_set(:@q, 3); rescue FrozenError => e; e.class; end)

# only the classes the receiver can hold gain the slot: one it never holds
# keeps its layout, its instance_variables and instance_variable_defined?
class U; def initialize = (@u = 1); end
u = U.new
p u.instance_variables, u.instance_variable_defined?(:@a), u.instance_variable_defined?(:@w)
puts u.inspect.sub(/0x\h+/, "0x")

# a receiver whose classes are not bounded here (a parameter) reaches any
def put_q(o) = o.instance_variable_set(:@q, 1)
class Q; end
put_q([Q.new, 1][0])
qs = [Q.new, K.new]
qs.each { |o| put_q(o) }
p qs.map { |o| o.instance_variable_get(:@q) }
