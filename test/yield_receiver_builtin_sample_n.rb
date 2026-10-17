# sample(n) on a yield receiver: tells a count (arg0 == TY_INT, subarray
# answer) apart from a random: keyword hash (arg0 is a Hash kind, single-
# element answer) purely from arg0's own TyKind -- confirmed empirically
# (a KeywordHashNode literal's inferred type is a Hash kind, not TY_INT),
# no AST-shape inspection needed.
def t = yield.sample(1)
p t { [1] }
p t { ["a"] }
