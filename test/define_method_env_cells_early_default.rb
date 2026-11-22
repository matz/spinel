# A call of a method before the definer that closes over the method's locals has run raises CRuby's NoMethodError,
# naming the receiver, also where a block parameter's default reads a captured local: through a receiver, through
# self with no receiver, and through a receiver that may be another class's instance.
class Early
  def self.mk(n)
    define_method(:m) { |a, b = n| [a, b] }
    :ok
  end
  def inside = (m(1) rescue $!.message)
end
class Other
  def m(a, b = 2) = [a, b]
end
e = Early.new
o = ARGV.size > 5 ? Other.new : e
p((e.m(1) rescue $!.message))
p e.inside
p((o.m(1) rescue $!.message))
# a call written as a variable (no receiver, no arguments) is a NameError, on an instance and on a class
class Early
  def self.mk_s(n)
    define_singleton_method(:g) { |a = [n]| a }
    define_method(:v) { |a = n| a }
    :ok
  end
  def self.h = g
  def vcall = v
end
p((Early.h rescue [$!.class, $!.message]))
p((e.vcall rescue [$!.class, $!.message]))
Early.mk_s(2)
p Early.h, e.vcall
Early.mk(5)
p e.m(1), e.inside, o.m(1), e.m(1, 9)
