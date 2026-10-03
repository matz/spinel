# The right operand of an Integer or Float op-assign that arrives boxed and
# is nil: CRuby raises the coercion TypeError ("nil can't be coerced into
# Integer"), and the conversion one for a shift count. A local and an
# attribute op-assign read the nil as 0, so `y += x` left y alone, `y *= x`
# made it 0, and the bitwise and shift forms computed with 0.

def t(s)
  r = yield
  puts "#{s}: #{r.inspect}"
rescue StandardError => e
  puts "#{s}: #{e.class}"
end
src = [1, nil, "s"]
x = src[1]
n = src[0]

y = 1
t("+=") { y += x; y }
t("-=") { y -= x; y }
t("*=") { y *= x; y }
t("/=") { y /= x; y }
t("%=") { y %= x; y }
t("|=") { y |= x; y }
t("&=") { y &= x; y }
t("^=") { y ^= x; y }
t("<<=") { y <<= x; y }
t(">>=") { y >>= x; y }
t("ok+=") { y += n; y }
z = 1.5
t("f+=") { z += x; z }
t("f*=") { z *= x; z }
t("fok") { z += n; z }

class K
  attr_accessor :v, :f, :p
  def initialize; @v = 1; @f = 1.5; @p = [3, "q"][0]; end
  def bump(d) = (@v += d)
  def fbump(d) = (@f += d)
  def orp(d) = (@p |= d)
end
k = K.new
t("@v+=") { k.bump(x) }
t("@f+=") { k.fbump(x) }
t("@p|=") { k.orp(x) }
t("@p|=ok") { k.orp(n) }
t("o.v+=") { k.v += x; k.v }
t("o.v|=") { k.v |= x; k.v }
t("o.f+=") { k.f += x; k.f }
a = [1, 2]
t("a[0]+=") { a[0] += x; a }
t("a[0]|=") { a[0] |= x; a }
t("a[0]<<=") { a[0] <<= x; a }
