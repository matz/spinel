# A constant written through Object (`Object::Y = v`, its || and && and
# operator forms, a multiple assignment's target, `Object.const_set`) is
# Object's: the load record holds it under the owner `::Y = v` has, from the
# top level, a class or module body or a block, through an alias of Object,
# and where the value opens a block of its own. A module's own `Object` is
# that module's, not Object's.
Object::A = 1
::B = 2
Object::C ||= 3
Object::D = 4
Object::D &&= 5
Object::E = 6
Object::E += 7
Object::F, ::G = 8, 9
Object.const_set(:H, 10)
p A, B, C, D, E, F, G, H, Object::A, ::H

class K
  Object::K1 = 11
  ::K2 = 12
  Object::K3 ||= 13
  Object.const_set(:K4, 14)
  p K1, K2, K3, K4
end
module M
  Object::M1 = 15
  [1].each { Object::M2 = 16 }
  p M1, M2
end

def run = yield
run { Object::R1 = 17 }
run { ::R2 = 18 }
run { Object::R3 ||= 19 }
p R1, R2, R3

O = Object
O::O1 = 20
O::O2 ||= 21
p O1, O2

Object::V1 = [1, 2].map { |v| v * 2 }
Object::V2 ||= [3].map { |v| v + 1 }
Object.const_set(:V3, [5].map { |v| v })
p V1, V2, V3

module N
  Object = Class.new
  Object::N1 = 22
  p N::Object::N1
  ::N2 = 23
end
p N2

# Object reopened after the write, and before it (through an alias): a class
# or struct so written is Object's whichever way the program reopens Object
Object::Y39 = Class.new { def hi = 1 }
S39 = Struct.new(:a) do
  def b = a + 1
end
p Y39.new.hi, S39.new(1).b
class Object
  def zz = 3
end
p Y39.new.hi, zz

class Object; end
O21 = Object
O21::Y21 = Class.new { def hi = 21 }
p Y21.new.hi
class Object
  p Y21.new.hi
end
