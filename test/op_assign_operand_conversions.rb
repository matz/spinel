# An `op=` takes its operand as the slot's own operator does. A Float slot's
# operator converts an Integer past 64 bits or a Rational to its double; the
# operand went into the C operator as it was and the C did not build. A
# global op-assigned an Integer past 64 bits widens to Bignum, as a local
# does, where it stayed an Integer slot the Bignum had no place in.

$s = 10
$s -= 2**63
p $s
f = 1.5
f += Rational(1, 2)
p f
g = 2.5
g *= 2**64
p g
@h = 0.5
@h -= Rational(1, 4)
p @h
$k = 1.0
$k += Rational(3, 4)
p $k
