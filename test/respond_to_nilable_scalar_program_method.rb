# nil answers respond_to? by the program's own NilClass and Object methods,
# also where the receiver is a String, an Integer or a Float that may be nil
class NilClass
  def t = 1
  private def pv = 2
end
class Object
  def zz = 3
end
class Foo; end

def mk(f) = f ? 7 : nil
def mf(f) = f ? 1.5 : nil
def ms(f) = f ? "s" : nil
def ma(f) = f ? [1] : nil
def mo(f) = f ? Foo.new : nil

[false, true].each do |f|
  x = mk(f); y = mf(f); s = ms(f); a = ma(f); o = mo(f)
  puts(x.respond_to?(:t) ? "x-yes" : "x-no")
  if s.respond_to?(:t) then puts "s-yes" else puts "s-no" end
  p y.respond_to?(:t), a.respond_to?(:t), o.respond_to?(:t)
  p x.respond_to?(:zz), s.respond_to?(:zz), a.respond_to?(:zz), o.respond_to?(:zz)
  # a private method answers only with include_all
  p x.respond_to?(:pv), x.respond_to?(:pv, true), s.respond_to?(:pv), s.respond_to?(:pv, true)
  p x.respond_to?(:t, false), s.respond_to?(:t, true)
  # builtin names keep their answers
  p x.respond_to?(:succ), s.respond_to?(:upcase), x.respond_to?(:even?)
end

class H
  def initialize(f) = (@v = f ? "q" : nil)
  def r = @v.respond_to?(:t)
end
p H.new(false).r, H.new(true).r
p "lit".respond_to?(:t), 3.respond_to?(:t), nil.respond_to?(:t)

# the first of NilClass, Object, Kernel and BasicObject that defines the
# name answers: a private NilClass method hides a public Object one
class Object; def sh = 1; end
class NilClass; private def sh = 2; end
module Kernel; def k3 = 1; end
class NilClass; private def k3 = 1; end
class BasicObject; def b2 = 1; end
class Object; private def b2 = 1; end
[false, true].each do |f|
  x = mk(f); y = mf(f); s = ms(f)
  p [x.respond_to?(:sh), y.respond_to?(:sh), s.respond_to?(:sh)]
  p [x.respond_to?(:k3), s.respond_to?(:k3), x.respond_to?(:b2), s.respond_to?(:b2)]
  p [x.respond_to?(:sh, true), s.respond_to?(:k3, true)]
end

# a boxed value answers by the same first class
[nil, 7, "s", 1.5, :y].each do |v|
  p [v.respond_to?(:sh), v.respond_to?(:b2), v.respond_to?(:b2, true), v.respond_to?(:zz)]
end

# an undef on NilClass hides Object's method; an accessor on Object counts
class Object
  def un = 1
  attr_writer :aw
  attr_reader :ar
end
class NilClass; undef_method :un; end
[false, true].each do |f|
  x = mk(f); s = ms(f)
  p [x.respond_to?(:un), s.respond_to?(:un), x.respond_to?(:aw=), s.respond_to?(:ar)]
end
[nil, 7, "s"].each { |v| p [v.respond_to?(:un), v.respond_to?(:aw=), v.respond_to?(:ar)] }

# an include_all only the run time knows runs once and decides nil's answer
class NilClass; private def pn = 1; end
$ia = 0
def ia(v) = ($ia += 1; v)
[false, true].each do |f|
  x = mk(f); s = ms(f)
  p [x.respond_to?(:pn, ia(true)), x.respond_to?(:pn, ia(false)), s.respond_to?(:pn, ia(true)), s.respond_to?(:pn, ia(false))]
  p(x.respond_to?(:pn, ia(true)) ? :yes : :no)
end
[nil, 7, "s"].each { |v| p [v.respond_to?(:b2, ia(true)), v.respond_to?(:pn, ia(true)), v.respond_to?(:pn, ia(nil))] }
p $ia
