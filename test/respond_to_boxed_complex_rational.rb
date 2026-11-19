# respond_to? on a boxed Complex or Rational
# A Complex read out of a mixed Array, or passed where an Integer is passed
# too, is boxed. respond_to? returned false for Complex and Rational methods
# on a boxed receiver, although calling the method worked.
c = [1, Complex(3, 4)][1]
r = [1, Rational(3, 4)][1]

p c.respond_to?(:real?), c.respond_to?(:abs), c.respond_to?(:imag), c.respond_to?(:arg)
p c.respond_to?(:+), c.respond_to?(:conj), c.respond_to?(:polar), c.respond_to?(:to_f)
p c.respond_to?(:to_s), c.respond_to?(:nope), c.respond_to?(:floor), c.respond_to?(:<)
p c.real?, c.abs, c.imag

p r.respond_to?(:real?), r.respond_to?(:numerator), r.respond_to?(:floor), r.respond_to?(:<)
p r.respond_to?(:between?), r.respond_to?(:to_f), r.respond_to?(:nope), r.respond_to?(:each)
p r.real?, r.numerator, r.floor

# the name held in a variable
m = :real?
p c.respond_to?(m), r.respond_to?(m)

# a parameter that one caller gives an Integer and another a Complex
def real_like?(z)
  return true unless z.respond_to?(:real?)
  z.real?
end
p real_like?(1), real_like?(1.5), real_like?(Complex(0, 1)), real_like?(Rational(1, 2)), real_like?("s")
