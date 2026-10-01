# spinel: int64
# A Float method's operand converts as CRuby converts it: an Integer past 64
# bits and a Rational become their nearest double. Float#divmod and
# Float#coerce took such an operand into an sp_float as it was, and the C did
# not build; so did a Rational duration given to sleep.

def big(n = 0)
  2**64 + n
end

p((-1.0).divmod(big))
p 3.5.divmod(big(1))
p 2.5.coerce(big)
p 7.5.divmod(Rational(1, 2))
p (-7.25).divmod(Rational(3, 4))
p 2.5.coerce(Rational(1, 4))
t = Process.clock_gettime(Process::CLOCK_MONOTONIC)
sleep(Rational(1, 100))
p Process.clock_gettime(Process::CLOCK_MONOTONIC) - t >= 0.01
# the conversions that already held stay as they were
p 2.0 ** Rational(1, 2)
p 1.5 + big
p 1.5.fdiv(big)
p Complex(2**62, 1)
