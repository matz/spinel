# A typed Array's index answer read by `& | ^` (or their op-assigns),
# `method`, `clone(freeze: ...)` or `sum(seed) { }` -- directly, through a
# local, a copy, a parameter, an ivar, a block parameter or a multiple
# assignment -- answers as nil does when it is nil (false / true / nil's
# Method / NoMethodError for `+`), and as the Integer when it is found.
ia = [3, 1]; sa = %w[a b]; fa = [1.5, 2.5]
m = ARGV.size == 0 ? 9 : 1
def t(l) = (print l, ": "; p yield) rescue puts("#{$!.class}: #{$!.message}")
t("&") { ia.index(m) & 1 }
t("|") { ia.index(m) | 2 }
t("^") { ia.rindex(m) ^ 3 }
t("(idx) &") { (ia.find_index(m)) & 1 }
t("method") { ia.index(m).method(:inspect).call }
i = ia.index(m)
t("local &") { i & 1 }
t("local method") { i.method(:to_s).call }
t("local +") { (i || 0) + 1 }
j = sa.index(ARGV.size == 0 ? "z" : "b")
t("str &") { j & 4 }
k = fa.index(ARGV.size == 0 ? 9.9 : 2.5)
t("flt |") { k | 8 }
t("plain +") { ia.index(3) + 1 }
t("found") { ia.index(1) & 1 }
w = ia.index(m); w &= 1
t("op-assign") { w }
def band(n) = n & 1
t("param") { band(ia.index(m)) }
@iv = ia.index(m)
t("ivar") { @iv | 2 }
q = ia.index(m); r = q
t("copy") { r ^ 1 }
t("block param") { ia.index(m).then { |k| k & 1 } }
c, d = ia.index(m), 1
t("masgn") { [c & 1, d] }
t("clone kw") { ia.index(m).clone(freeze: true) }
t("sum block") { ia.sum(ia.index(m)) { _1 } }
