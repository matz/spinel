# A Struct instance keeps an ivar of its own beside its members, as in
# CRuby: instance_variable_get reads it back, typed or boxed, and a member
# stays no ivar (#2849). A Data instance is frozen: instance_variable_set
# raises FrozenError. The typed read of a Struct's ivar was typed boxed
# and emitted unboxed (the C did not build), a boxed set was dropped, and
# a Data set went through.

S = Struct.new(:a)
x = S.new(1)
x.instance_variable_set(:@z, 3)
p x.instance_variable_get(:@z), x.a
p x.instance_variable_get(:@a)

y = [S.new(2), 0][0]
y.instance_variable_set(:@z, 4)
p y.instance_variable_get(:@z), y.a
p y.instance_variable_get(:@a)

T = Struct.new(:a) do
  def tag = (@tag ||= "t")
end
t = T.new(1)
p t.tag, t.instance_variable_get(:@tag)

D = Data.define(:a)
d = D.new(a: 1)
p d.instance_variable_get(:@a), d.instance_variables
p(begin; d.instance_variable_set(:@z, 3); rescue FrozenError => e; e.class; end)
e = [D.new(a: 2), 0][0]
p(begin; e.instance_variable_set(:@q, 3); rescue FrozenError => er; er.class; end)

# a boxed read of an Integer ivar from an object whose class lacks it
class K0; end
class L0; def initialize = (@n = 1); end
p [K0.new, L0.new].map { |o| o.instance_variable_get(:@n) }
