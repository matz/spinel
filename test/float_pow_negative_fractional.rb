# spinel: not-cruby -- a typed negative base to a fractional power raises Math::DomainError (docs/limitations.md); CRuby answers a Complex
# Negative Float ** fractional exponent raises Math::DomainError loudly
# (CRuby promotes to a Complex; documented divergence in
# docs/limitations.md -- expected file pins the spinel behavior).
begin
  p((-2.0) ** 0.5)
rescue => e
  puts "#{e.class}: raised"
end
p(2.0 ** 0.5)
p((-2.0) ** 2.0)
p((-8.0) ** (1.0 / 3)) rescue puts "raised2"

p((-8).pow(0.5)) rescue puts "raised3"

# an infinite or huge exponent is integral, as CRuby's round test finds it
r = Rational(-1, 4)
i = -8
p((-8.0) ** Float::INFINITY, (-8.0) ** 1e300, r ** Float::INFINITY, i.pow(Float::INFINITY))
