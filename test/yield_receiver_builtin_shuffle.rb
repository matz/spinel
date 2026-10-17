# shuffle on a yield receiver, same family as sort_bang's own test.
# shuffle's own output is randomized, so this prints the sorted shuffle
# result (deterministic) rather than the raw value, matching the existing
# sample_random_boxed.rb test's own convention for randomized builtins.
def t = yield.shuffle
p(t { [3, 1, 2] }.sort)
p(t { ["b", "a"] }.sort)
