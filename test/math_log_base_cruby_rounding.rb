# Math.log(x, base) as CRuby computes it
# CRuby's rb_math_log divides log2(x) by log2(base), after its own checks
# for a zero argument and a zero base. Spinel divided log(x) by log(base),
# which differs in the last digit (Math.log(7.75, Math::E)), and
# Math.log(0, 0.5) was Infinity where CRuby answers -Infinity.
p Math.log(7.75, Math::E), Math.log(1e-8, Math::E), Math.log(7.75, 10)
p Math.log(3.3, 0.5), Math.log(12345.678, 7.5), Math.log(8, 2), Math.log(100, 10)

# a zero argument, a zero base, base 1
p Math.log(0, 0.5), Math.log(0, 2), Math.log(0, 1), Math.log(0, 0)
p Math.log(2, 0), Math.log(Float::INFINITY, 0), Math.log(1, 0.5)
p Math.log(2, 1), Math.log(1, 1)

# infinities and NaN
p Math.log(Float::INFINITY, 2), Math.log(Float::INFINITY, 0.5), Math.log(5, Float::INFINITY)
p Math.log(0, Float::NAN), Math.log(Float::NAN, 2), Math.log(2, Float::NAN)

# a negative argument or base
begin
  Math.log(2, -1)
rescue Math::DomainError => e
  p e.message
end
begin
  Math.log(-1, 2)
rescue Math::DomainError => e
  p e.message
end

# through a method, and with an argument read out of a mixed Array
def log_of(x, b) = Math.log(x, b)
p log_of(7.75, Math::E), log_of(0, 0.5)
x = [1, 7.75][1]
p Math.log(x, 3)
