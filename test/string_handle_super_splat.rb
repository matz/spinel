# A String variable `super` hands on through a splat -- an element of a
# splatted Array, or an argument written after the splat -- is the caller's
# own String, as in CRuby: the method the element lands on appends to it.
# The binding was refused at compile time, and a parameter of the parent
# that gathers a rest appended to a copy (#6179).

X = "x" * 40

# an element of an inline Array literal
class A1; def m(a) = (a << X; nil); end
class A2 < A1; def m(x) = super(*[x]); end
s = +"a"; A2.new.m(s); p s.size

# an element of a local Array, beside another argument
class B1; def m(a, b) = (a << X; b); end
class B2 < B1; def m(x) = (r = [x]; super(*r, 1)); end
s = +"b"; p B2.new.m(s); p s.size

# an Array built by `<<`, into optionals, a rest and a keyword
class C1; def m(a, b = 1, *r, k: 0) = (a << X; k); end
class C2 < C1; def m(x) = (r = []; r << x; super(*r, k: 2)); end
s = +"c"; p C2.new.m(s); p s.size

# an argument written after an empty splat
class D1; def m(a, b) = (b << X; a); end
class D2 < D1; def m(x) = (v = +"d"; p super(*[], x, v); p v.size); end
D2.new.m(1)

# the parent gathers it into its rest
class E1; def m(*r) = (r.each { |e| e << X }; nil); end
class E2 < E1; def m(x, y) = super(*[x, y]); end
s = +"e"; t = +"f"; E2.new.m(s, t); p s.size, t.size

# through an included module and a prepended one
module F1; def m(a, b) = (b << X; a); end
class F2; include F1; def m(x, y) = (t = [y]; super(x, *t)); end
s = +"g"; p F2.new.m(1, s); p s.size
module G1; def m(x) = super(*[x]); end
class G2; prepend G1; def m(a) = (a << X; nil); end
s = +"h"; G2.new.m(s); p s.size

# a class method, two levels of super, a block and a lambda around the super
class H1; def self.m(a) = (a << X; nil); end
class H2 < H1; def self.m(x) = super(*[x]); end
s = +"i"; H2.m(s); p s.size
class I1; def m(a) = (a << X; nil); end
class I2 < I1; def m(x) = super(*[x]); end
class I3 < I2; def m(y) = super(*[y]); end
s = +"j"; I3.new.m(s); p s.size
class J1; def m(a) = (a << X; nil); end
class J2 < J1; def m(x) = [1].each { |_| super(*[x]) }; end
s = +"k"; J2.new.m(s); p s.size
class K1; def m(a) = (a << X; nil); end
class K2 < K1; def m(x) = (f = -> { super(*[x]) }; f.call); end
s = +"l"; K2.new.m(s); p s.size

# a local the parameter is copied to, and a block's parameter
class L1; def m(a) = (a << X; nil); end
class L2 < L1; def m(x) = (v = x; super(*[v])); end
s = +"m"; L2.new.m(s); p s.size
L2.new.m(+"n")
class M1; def m(a) = (a << X; nil); end
class M2 < M1; def m(x) = [x].each { |q| super(*[q]) }; end
s = +"o"; M2.new.m(s); p s.size

# a call's splat of a local Array that holds a parameter
class N1; def m(a) = (a << X; nil); end
class N2; def n(x) = (r = [x]; N1.new.m(*r)); end
s = +"p"; N2.new.n(s); p s.size
class O1; def m(a) = (a << X; nil); end
class O2 < O1; def m(x) = (v = x; O1.new.m(*[v])); end
s = +"q"; O2.new.m(s); p s.size
