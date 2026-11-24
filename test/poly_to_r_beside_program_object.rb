# Beside a program class's to_r, to_c and rationalize, a boxed receiver
# holding nil, an Integer, a Float or a Rational takes the builtin
# conversion. nil, 3 and Rational(1, 3) raised NoMethodError for to_r.
class Foo
  def to_r = Rational(1, 3)
  def to_c = Complex(1, 2)
  def rationalize(eps = nil) = Rational(2, 3)
end

[Foo.new, nil, 3, 1.5, Rational(1, 3)].each { |e| p e.to_r }
[Foo.new, nil, 3, 1.5].each { |e| p e.to_c }
[Foo.new, nil, 3, 0.1, Rational(1, 3)].each { |e| p e.rationalize }
