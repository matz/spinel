# A local holding the class a Class.new in a method makes, evaluated more
# than once, lists that class's own methods, as a constant's class does.
# (A list of two names or more is refused: CRuby's order follows its
# method table, test/reject/class_new_site_local_methods_order.rb.)
class Base
  def q = 0
end

def own(n)
  k = Class.new { def self.f = 1; def v = 2 }
  [n, k.f, k.new.v, k.instance_methods(false), k.public_instance_methods(false)]
end
p own(1), own(2)

def kinds
  k = Class.new { def v = 1; private def pv = 2; protected def pr = 3 }
  [k.public_instance_methods(false), k.private_instance_methods(false), k.protected_instance_methods(false)]
end
p kinds, kinds

def maybe(a)
  k = a ? Class.new { def v = 1 } : nil
  k&.instance_methods(false)
end
p maybe(true), maybe(false)

def either(a)
  k = a ? Class.new(Base) { def v = 1 } : Base
  k.instance_methods(false)
end
p either(true), either(false)

def both
  [Class.new { def v = 1 }, Class.new { def w = 1 }].map { |k| k.instance_methods(false) }
end
p both, both
