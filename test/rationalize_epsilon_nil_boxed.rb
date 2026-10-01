# spinel: int64
# NilClass#rationalize takes an epsilon of any kind, ignores it and answers
# (0/1); it is still evaluated. A boxed receiver -- a value read out of a
# mixed Array, a local that may be nil -- answers rationalize as its kind
# does: nil and an Integer ignore the epsilon, a Float and a Rational answer
# the simplest rational within it, and a Float with none the simplest that
# rounds back to it, which is not its exact to_r.
n = 0
p nil.rationalize(0.1), nil.rationalize(Rational(1, 100)), nil.rationalize("any")
x = nil
p x.rationalize((n += 1; 0.01)), n
vals = [nil, 3, 0.1, 0.333, Rational(1, 3)]
vals.each { |v| p [v.rationalize, v.rationalize(Rational(1, 100)), v.rationalize(0.01)] }
z = ARGV.size > 5 ? 1.5 : nil
p z.rationalize(Rational(1, 10)), z.to_r
y = [0.1, nil][0]
p y.rationalize, y.to_r == 0.1.to_r
