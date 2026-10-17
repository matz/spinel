# rotate on a yield receiver, same family as sort_bang's own test.
def t = yield.rotate
p t { [1, 2, 3] }
p t { ["a", "b", "c"] }
