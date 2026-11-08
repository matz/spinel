# A literal block on a split call is copied into both branches: break, and the branch that does not run leaves no trace.
e = {}
n = {a: 1}
p([1, 2, 3].index(**e) { |s| break s * 10 if s == 2 })
p([1, 2, 3].index(**n) { |s| break s if s > 1 })
p([1, 2, 3].index(**{}) { |s| break s * 100 if s == 1 })
