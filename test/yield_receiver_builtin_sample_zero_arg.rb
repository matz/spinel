# sample (0-arg, one element) on a yield receiver: always the element
# kind, no ambiguity (unlike sample(n), see
# yield_receiver_builtin_sample_n.rb). Deterministic property (.class via
# a check rather than the raw random element) would be simpler, but a
# single-element Array makes the raw value deterministic too.
def t = yield.sample
p t { [1] }
p t { ["a"] }
