# An Integer Array's sum(seed) with a Float seed that may be nil, and is
# nil: over no elements the fold answers the nil seed, over elements nil's
# own `+` raises NoMethodError. A Float seed that is no nil still adds the
# Integer total to it in floating point, keeping -0.0 over no elements.
fs = [1.5, -0.0]
at = [0, 5, 1]
ia = [1, 2]
e = [7]; e.clear
def t(l) = (print l, ": "; p yield) rescue puts("#{$!.class}")
3.times do |i|
  s = fs[at[i]]
  t("e #{i}") { e.sum(s) }
  t("ia #{i}") { ia.sum(s) }
end
r = ia.sum(fs[0])
p r, r + 1
