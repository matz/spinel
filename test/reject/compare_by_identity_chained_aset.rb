# An index assignment chained onto compare_by_identity (see
# compare_by_identity_chained.rb).
h = { a: 1 }
h.compare_by_identity[:b] = 2
p h
