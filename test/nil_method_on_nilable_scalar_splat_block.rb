# A String, Integer or Float that may be nil, called with a splat or a
# block, runs NilClass's method for nil, and Object's method for the rest.
class Object
  def w(a = 1, *r, k: 2) = [:obj, a, r, k]
  def q(*r, &b) = [:oq, r, b ? b.call : :nob]
  def s(a, b = 2) = a + b
  def v(*r) = r.size
end
class NilClass
  def w(a = 10, *r, k: 20) = [:nil, a, r, k]
  def q(*r, &b) = [:nq, r, b ? b.call : :nob]
  def s(a, b = 9) = -a - b
  def v(*r) = -1
  def z(*r) = [:nz, r]
end
class Foo; def s(a, b = 0) = [:foo, a, b]; end

def mk(f) = f ? 7 : nil
def ms(f) = f ? "s" : nil
def mf(f) = f ? 1.5 : nil
def side; $n = ($n || 0) + 1; [3]; end

arr = [1, 2]
[false, true].each do |f|
  p mk(f).w(*arr)
  p ms(f).w(*arr)
  p mf(f).w(*arr)
  p mk(f).w(*side)
  p mk(f).w(0, *arr, k: 9)
  p mk(f).w { 1 }
  p ms(f).w(4) { 1 }
  p mk(f).q(1) { :b }
  p ms(f).q { :c }
  p mk(f).s(*[1])
  p mk(f).s(*[1, 3])
  p ms(f).v(*nil)
  p mk(f).v(*1)
  p ms(f).v(*"a")
  mk(f).v(*[1])
end
p $n

# a NilClass-only method: the rest raise NoMethodError
p mk(false).z(*arr)
begin; p mk(true).z(*arr); rescue NoMethodError => e; p e.class; end
begin; p mk(true).z { 1 }; rescue NoMethodError => e; p e.class; end

# a boxed receiver: a splat into Object's method, its operand read once
o = []
[nil, 4, "y", Foo.new].each { |x| o << x.s(*[1, 2]) }
p o
[nil, 4, "s"].each { |x| p x.w(*side) }
p $n
[nil, 4].each { |x| p x.q(*[1, 2], *[3]) }

begin; mk(true).s(*[1, 2, 3]); rescue ArgumentError => e; p e.message; end
begin; mk(false).s(*[]); rescue ArgumentError => e; p e.message; end
h = { a: 1 }
p mk(false).q(*[1], **h)
p mk(false).w(*arr) { :blk }

# an argument ahead of a splat keeps the value it had before the splat's
# operand ran
class Object; def ah(a, *r) = [:o, a, r]; end
class NilClass; def ah(a, *r) = [:n, a, r]; end
def bump; $g = 99; @iv = 50; [2]; end
[false, true].each { |f| $g = 1; p mk(f).ah($g, *bump) }
[nil, 4, "s"].each { |x| $g = 1; p x.ah($g, *bump) }
[nil, 4].each { |x| @iv = 1; p x.ah(@iv, *bump) }

# a keyword beside a splat or a block
# for the non-nil value
class Object; def kw(*r, **k, &b) = [:o, r, k, b ? b.call : 0]; end
class NilClass; def kw(*r, **k, &b) = [:n, r, k, b ? b.call : 0]; end
p mk(true).kw(*arr, k: 1)
p mk(true).kw(k: 2) { 1 }
