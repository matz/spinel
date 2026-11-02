# A class, module or Class.new value bound to a constant target of a multiple
# assignment dispatches its methods, and the constant names it at run time
# (name, to_s and inspect answer the constant's name), in a body or at the top
# level, at a root-scoped target (`::A`), through `Object::` and with a target
# that takes a non-class. A `Class.new { ... }` element is not named when the
# program is compiled, so an error message that names its class or a subclass
# of it still sees the anonymous class.
A, B = Class.new { def hi = 1 }, Class.new { def hi = 2 }
p A.new.hi, B.new.hi, A.name, B.name

::C, x = Class.new { def hi = 3 }, 5
p C.new.hi, C.name, x

::D, ::E = Class.new { def hi = 4 }, Class.new { def hi = 5 }
p D.new.hi, E.new.hi, D.name, E.name

y, F = 7, Class.new { def hi = 6 }
p y, F.new.hi, F.name

G, = [Class.new { def hi = 7 }]
p G.new.hi, G.name

H, I = [Class.new { def hi = 8 }, Class.new { def hi = 9 }]
p H.new.hi, I.new.hi, H.name, I.name

J, z = Module.new { def self.hi = 10 }, 1
p J.hi, J.name

class K
  L, w = Class.new { def hi = 11 }, 0
  ::M, v = Class.new { def hi = 12 }, 0
  p L.new.hi, L.name, M.new.hi, M.name
end
p K::L.name, M.name

# `Object::X` names X, as `::X` does, in a plain write and in a target
Object::P = Class.new { def hi = 13 }
Object::Q, o = Class.new { def hi = 14 }, 0
p P.name, Q.name

# A plain path write names the class under the path's receiver, not the body
module A21; end
def mk21 = Class.new { def hi = 21 }
module B21
  A21::X21 = mk21
end
p A21::X21.name

# A rescued exception's class carries its name, not a Class.new class's id
e22 = begin; raise ArgumentError; rescue => x22; x22; end
X22, Y22 = e22.class, Class.new { def hi = 22 }
p X22, Y22.name

# Object's own body names its constants bare
class Object
  self::X23, $v23 = Class.new { def hi = 23 }, 0
end
p X23.name

# A class reopened in a `class << self` body is self there
class A24; end
class H24
  class << self
    class ::A24
      self::C24, z24 = Class.new { def hi = 24 }, 0
    end
  end
end
p A24::C24.name
