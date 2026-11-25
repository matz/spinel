# A call whose count a candidate method cannot take raises that
# method's ArgumentError once its arguments have run; the arguments are
# not converted into its parameters, which the call never reaches. A
# String for an Integer parameter was refused at compile time, or written
# as C that does not compile in a Class-value dispatch arm.
class A; def self.m(a, b = 1, k: 0) = [:a, a, b, k]; end
class B; def self.m(a) = [:b, a + 1]; end
$log = []
def v(x) = ($log << x; x)
def go(k)
  k.m(v("s"), v(:sym))
rescue ArgumentError => e
  [:err, e.message]
end
p go(Object.const_get(%w[A][0]))
p go(Object.const_get(%w[B][0]))
p Object.const_get(%w[B][0]).m(1)
p $log
begin
  B.m("s", :sym)
rescue ArgumentError => e
  p e.message
end
def go2(k)
  r1 = k.m(nil)
  r2 = k.m("s", :sym)
  [r1, r2]
end
p go2(Object.const_get(%w[A][0]))
