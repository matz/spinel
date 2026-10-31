# A Range holds Integer, Float or String bounds, so a Rational bound has
# nowhere to live: the Integer Range kept only its integer part.
# `(1r/2..2) === 1r/4` answered true (CRuby false), `(1r..3).begin`
# answered 1 (CRuby (1/1)) and `(1r..3).each` iterated where CRuby raises
# "can't iterate from Rational". Each use is refused.
q = 1r / 4
p((1r / 2..2) === q)
r = (1r..3)
p r.begin
case q
when 1r / 2..2 then p :in
end
