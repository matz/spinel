# Ruby decides the sign at +-0.0 itself; at a negative integer CRuby takes
# it from its libm, which differs between machines, so only the value is
# printed there (spinel answers -1, Ruby's documented answer)
p Math.lgamma(-0.0)
p Math.lgamma(0.0)
p Math.lgamma(-1.0)[0]
p Math.lgamma(-2.0)[0]
