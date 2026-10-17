# sample(random: g) (the 1-argument, keyword-hash form) on a yield
# receiver: answers the element kind, same as a blockless sample() --
# arg0 is a Hash kind here, not TY_INT, which is what tells it apart from
# sample(n)'s count form. Checks membership rather than the exact value,
# since Spinel's own Random algorithm need not produce CRuby's same
# sequence for the same seed.
def t = yield.sample(random: Random.new(1))
p([1, 2, 3].include?(t { [1, 2, 3] }))
p(["a", "b", "c"].include?(t { ["a", "b", "c"] }))
