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

# stores of two kinds into two elements, each into its own
def m5(*r) = (r[0] << 7; r[-1] << :s)
a = [1]; b = [+"t"]; m5(a, 0, b); p [a.sum, b]

# a rest handed on through a bare super
class P1; def rr(*r) = r[0] << 6; end
class S4 < P1; def rr(*r) = super; end
class S5 < S4; def rr(*r) = super; end
class S7 < P1; def rr(x, *r) = super; end
a = [+"s"]; S4.new.rr(a); p a
a = [+"s"]; S5.new.rr(a); p a
a = [+"s"]; b = [+"t"]; S7.new.rr(b, a); p [a, b]

# a rest the body reorders: a literal place no longer names one value
def m6(*r) = (r.shift; r[0] << 8)
a = [+"s"]; b = [+"t"]; m6(a, b); p [a, b]
a = [+"s"]; b = [+"t"]; y2(a, b) { |*r| r.reverse!; r[0] << 9 }; p [a, b]

# a keyword hash the rest takes at its tail is one more value
def m7(*r) = r[-2] << 10
a = [+"s"]; m7(a, k: 1); p a

# a value of a kind the run time decides, handed by a bare super onto a
# positional, is the push the positional takes: it stays typed
W8 = ARGV.size > 5 ? 1 : +"z"
class R8; def m(*r) = r[0] << W8; end
class R9 < R8; def m(x, *r) = super; end
class H8; attr_accessor :v; end
h = H8.new; h.v = [+"s"]; R9.new.m(h.v); p h.v

# a bare super lays an element of the parent's rest on the child's post,
# and on a positional of a child without a rest
class P10; def rr(*r) = r[1] << 11; end
class S10 < P10; def rr(*r, z) = super; end
class S11 < P10; def rr(x, y) = super; end
a = [+"s"]; b = [+"t"]; S10.new.rr(a, b); p [a, b]
a = [+"s"]; b = [+"t"]; S11.new.rr(a, b); p [a, b]
