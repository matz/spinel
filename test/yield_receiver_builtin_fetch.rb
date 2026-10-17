# fetch(i) (the 1-argument, KeyError-on-miss, blockless form) on a yield
# receiver: always the element kind, never argument-dependent.
def t = yield.fetch(0)
p t { [1, 2, 3] }
p t { ["a", "b", "c"] }

# A direct (non-yield) block form, unaffected by the row above: the
# blockless row answering fetch(i) must be BF_NONE, not BF_ANY, or it
# shadows the separate hand-written rule for fetch(i) { ... } (whose
# answer compares the block's own tail type against the element kind) --
# a fresh review caught this as BF_ANY, which broke ordinary Array#fetch
# with a block (two existing corpus tests, array_fetch_block_value_setup.rb
# and hash_default_array_fetch_concat_wave9.rb, regressed from it).
p [1, 2].fetch(9) { |i| "s#{i}" }
