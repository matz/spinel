# sort (argc=0) is both hand-written above the generic fallback AND
# answerable by it (its bop_rows row already has a concrete BOPR_SELF
# result) -- the hand-written arm must still be the one that answers, not
# the generic one, though both would agree here. Proves the ordering
# didn't regress when the generic fallback was added after it.
def t = yield.sort
p t { [3, 1, 2] }
p t { ["b", "a"] }
