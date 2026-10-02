# Rational ** a boxed exponent -- a block parameter a lambda captures, a
# variable that may hold an Integer or a Rational -- goes through
# sp_poly_pow, whose answer is boxed: a Rational for an Integer or an
# integer-valued Rational exponent, a Float otherwise. It was typed Float,
# and the boxed answer was handed to sp_box_float, which did not compile.
[Rational(-1, 1), Rational(2, 1), 3, Rational(1, 2)].each do |e|
  f = -> { Rational(1, 4) ** e }
  p f.call
end

e = Rational(2, 1)
e = 3 if ARGV.size > 5
p(Rational(1, 2) ** e)

[Rational(-1, 1), Rational(-3, 1)].each do |x|
  begin
    p(Rational(0, 1) ** x)
  rescue ZeroDivisionError => err
    p err.message
  end
end
