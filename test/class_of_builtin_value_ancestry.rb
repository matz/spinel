# A builtin value's #class (`Rational(3, 4).class`) has its builtin's
# superclass and ancestors: Numeric, and the modules Comparable and
# Enumerable, as the constant (`Rational`) has.
#
# A builtin's reopening includes and prepends modules on the value's class too
module Mx; end
module Px; end
class Integer; include Mx; end
class String; prepend Px; end

def show(v)
  k = v.class
  p [k, k.superclass, k.ancestors, k.include?(Comparable), k.include?(Enumerable), k < Comparable, k.included_modules]
end
show(1)
show(1.5)
show(Rational(1, 3))
show(Complex(1, 2))
show("s")
show(:s)
show(Time.at(0))
show([1])
show({ a: 1 })
show(1..2)
show(nil)
p 1.class.ancestors, "s".class.superclass, Rational(1, 2).class < Numeric
p Object.new.class.ancestors
p 1.class.include?(Mx), "s".class < Px

# A rescued exception's class keeps its own ancestry
begin
  [].fetch(9)
rescue => e
  p e.class.superclass, e.class.ancestors.include?(StandardError)
end
