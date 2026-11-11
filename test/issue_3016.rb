# poles at non-positive integers are exactly Infinity; the sign is 1 at 0.0.
# At a negative integer spinel answers -1, Ruby's documented answer, but
# CRuby takes that sign from its libm and it differs between machines, so
# only the value is printed there
p Math.lgamma(-1.0)[0]
p Math.lgamma(-2.0)[0]
p Math.lgamma(0.0)
p Math.lgamma(-3.0)[0]
p Math.lgamma(-10.0)[0]
# finite values: spinel's own series diverges in the last ULPs from libm by
# design (machine-independent golden output), so check only sign + finiteness
r = Math.lgamma(5.0);  p [r[0].finite?, r[1]]
r = Math.lgamma(-0.5); p [r[0].finite?, r[1]]
r = Math.lgamma(0.5);  p [r[0].finite?, r[1]]
