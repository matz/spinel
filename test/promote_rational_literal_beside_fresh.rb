# spinel: gc-stress
# A Rational literal is an sp_Rational value, but --int-overflow=promote
# holds a Rational boxed, and boxes the literal each time it is read: an
# allocation. An operand beside it that is a fresh Rational -- a call's
# answer, an operator's -- was left unrooted while the literal was boxed,
# and the arithmetic read a freed one (DateTime#ajd's
# `Rational(@jd) + day_fraction - offset - 0.5r` answered (0/1)).
class Clock
  def initialize(h, m) = (@h = h; @m = m)
  def frac = Rational(@h * 60 + @m, 1440)
  def shift = Rational(3, 8)
  def ajd(jd) = Rational(jd) + frac - shift - 0.5r
  def half_after = 1/2r + frac
  def scaled = (frac + shift) * 3r
  def mixed(x) = [frac - 1/4r, 2r * frac, x + frac + 1r]
end

c = Clock.new(12, 30)
p c.ajd(2461071), c.half_after, c.scaled, c.mixed(Rational(1, 3))
s = 0r
200.times { |i| s += Clock.new(i % 24, i % 60).ajd(i) - 0.25r }
p s
