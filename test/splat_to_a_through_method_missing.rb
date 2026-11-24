# A splat converts its operand as CRuby's rb_check_funcall does: a class with
# no #to_a of its own is asked through method_missing. respond_to? (or
# respond_to_missing?) is asked when the class defines it; with neither,
# method_missing(:to_a) runs and a NoMethodError out of it means "no #to_a".
# An Array answer spreads, nil spreads the object itself, anything else is a
# TypeError; a nil operand spreads nothing.
class Conv
  def respond_to_missing?(name, priv = false) = name == :to_a || super
  def method_missing(name, *args)
    if name == :to_a
      puts "converted"
      return [:a, :b]
    end
    super
  end
end

class Bare
  def method_missing(name, *args) = name == :to_a ? [:x, :y] : super
end

class Ghost
  def method_missing(name, *args) = name == :to_a ? raise(NoMethodError, "no") : super
end

class Loud
  def method_missing(name, *args) = name == :to_a ? raise(ArgumentError, "inner") : super
end

class NoAnswer
  def respond_to_missing?(name, priv = false) = false
  def method_missing(name, *args) = name == :to_a ? [:never] : super
end

class NilAnswer
  def method_missing(name, *args) = name == :to_a ? nil : super
end

class NotArray
  def method_missing(name, *args) = name == :to_a ? "str" : super
end

class SubConv < Conv; end

def relay(x) = yield(*x, 1)
def count(*a) = a.size

c = Conv.new
p [*c]
p [0, *c, 9]
p count(*c)
p relay(c) { |a, b, d| [a, b, d] }
p [*Bare.new]
p [*Ghost.new].map(&:class)
p [*NoAnswer.new].map(&:class)
p [*NilAnswer.new].map(&:class)
p [*c, *Bare.new]
p [*SubConv.new]
x = ARGV.empty? ? nil : Conv.new
p [*x]
y = ARGV.empty? ? Conv.new : nil
p [*y]
a, b = *c
p [a, b]
p(case :b when *c then :hit else :miss end)

begin
  p [*Loud.new]
rescue ArgumentError => e
  p [e.class, e.message]
end
begin
  p [*NotArray.new]
rescue TypeError => e
  p [e.class, e.message]
end
