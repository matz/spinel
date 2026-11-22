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
Early.mk(5)
p e.m(1), e.inside, o.m(1), e.m(1, 9)
