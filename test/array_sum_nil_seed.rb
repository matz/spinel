# A typed Array's sum(seed) with an Integer seed that may be nil, and is
# nil: over elements nil's own `+` raises NoMethodError (not the seed's
# conversion TypeError), over none the fold answers the nil seed. A typed
# Array's index answer is such a seed.
ia = [3, 1]; e = []; fe = []; fa = [1.5]
x = [1].tap(&:shift).first
x = 5 if ARGV.size > 0
m = ARGV.size == 0 ? 9 : 1
def t(l) = (print l, ": "; p yield) rescue puts("#{$!.class}: #{$!.message}")
t("e") { e.sum(x) }
t("ia") { ia.sum(x) }
t("fa") { fa.sum(x) }
t("fe") { fe.sum(x) }
t("idx") { ia.sum(ia.index(m)) }
t("idx empty") { e.sum(ia.index(m)) }
t("fidx") { fa.sum(ia.index(m)) }
s = e.sum(ia.index(m))
p s.nil?, (s || 0) + 1
