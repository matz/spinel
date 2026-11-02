# A class, module or struct class written through Object (`Object::Q = v`,
# `Object.const_set`, or through an alias of Object) is read by its bare name
# as the one `::Q = v` makes: the load record holds the write under Object's
# owner, so the read finds the constant.
Object::Q1 = Class.new { def hi = 1 }
p Q1.new.hi
p Object::Q1.new.hi
p ::Q1.new.hi
p Q1.name

Object::Q2 = Struct.new(:a)
p Q2.new(2).a
p Object::Q2.new(3).a

Object.const_set(:Q3, Class.new { def hi = 4 })
p Q3.new.hi
p Object::Q3.new.hi

O = Object
O::Q4 = Class.new { def hi = 5 }
p Q4.new.hi

Object::Q5 = Module.new { def self.hi = 6 }
p Q5.hi
p Object::Q5.hi
module Mx; end
Object::Q6 = Mx
p Q6 == Mx

module A
  Object::Q7 = Class.new { def hi = 7 }
  p Q7.new.hi
end
p Q7.new.hi

Object::Q8, q = Class.new { def hi = 8 }, 0
p Q8.new.hi, Q8.name
Object::Q9, Object::Q10 = Class.new { def hi = 9 }, Class.new { def hi = 10 }
p Q9.new.hi, Q10.new.hi, Q9.name, Q10.name

# A write through Object inside a module body defines a top-level constant
module M3
  Object::P3 = Class.new { def hi = 3 }
end
p P3.name, P3.new.hi
