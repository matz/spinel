# A store through an element of a rest parameter (`r[0] << 1`) widens the
# Array the call laid into the rest, for a method's rest (through each
# caller's binding) as for a block's or a proc's.

def m1(*r) = r[0] << 1
def m2(i, *r) = r[0].push(i)
def m3(*r, z) = r[-1] << z
def y2(a, b) = yield(a, b)

a = [+"s"]; m1(a); p a
a = [+"s"]; m2(2, a); p a
a = [+"s"]; b = [+"t"]; m3(a, b, 3); p [a, b]
a = [+"s"]; m1(*[a]); p a
a = [+"s"]; y2(0, a) { |i, *r| r[0] << i }; p a
a = [+"s"]; pr = proc { |*r| r[1] << 4 }; pr.call(0, a); p a

class D
  define_method(:m4) { |*r| r[0] << 5 }
end
a = [+"s"]; D.new.m4(a); p a
