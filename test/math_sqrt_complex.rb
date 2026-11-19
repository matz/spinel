# Math.sqrt of a Complex returns a Complex, as CRuby's rb_math_sqrt computes
# it from the absolute value; the sign of the imaginary part gives the sign
# of the result's. Spinel raised RangeError for a Complex and TypeError for
# a boxed one.

p Math.sqrt(Complex(3, 4)), Math.sqrt(Complex(-4, 0)), Math.sqrt(Complex(0, 0)), Math.sqrt(Complex(0, 2))
p Math.sqrt(Complex(1.5, -2.5)), Math.sqrt(Complex(-3, 4)), Math.sqrt(Complex(-3, -4))
p Math.sqrt(3 + 4i), Math.sqrt(-1i), Math.sqrt(Complex.polar(2.0, 1.0))

# a negative zero imaginary part, Infinity, NaN and very large or small parts
p Math.sqrt(Complex(-4.0, -0.0)), Math.sqrt(Complex(-4.0, 0.0)), Math.sqrt(Complex(4, -0.0))
p Math.sqrt(Complex(Float::INFINITY, 1)), Math.sqrt(Complex(1, Float::NAN)), Math.sqrt(Complex(-Float::INFINITY, -1.0))
p Math.sqrt(Complex(1e300, 1e300)), Math.sqrt(Complex(1e-300, -1e-300))

# a boxed argument: a Complex, an Integer or a Float
def pick(k)
  case k
  when 0 then Complex(3, 4)
  when 1 then 16
  when 2 then 2.25
  else Complex(-1.5, -0.0)
  end
end
[0, 1, 2, 3].each { |k| p Math.sqrt(pick(k)) }

def pick_i(f) = f ? 3.i : 4.0
p Math.sqrt(pick_i(true)), Math.sqrt(pick_i(false))

# Math under another name, as the cmath gem calls it
module Roots
  RealMath = Math
  module_function

  def root(z) = RealMath.sqrt(z)
end
p Roots.root(Complex(-9, 0)), Roots.root(25), Roots.root(Complex(0.0, -8.0))

# a negative real is still out of the domain
begin
  Math.sqrt(pick(1) - 20)
rescue Math::DomainError => e
  p e.message
end
