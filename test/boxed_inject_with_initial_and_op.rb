# inject / reduce with an initial value and an operator Symbol on a receiver
# typed as more than an Array (a write that never runs widens it): the fold
# starts at the initial value, as the one-argument form folds from the first
# element (it read the receiver as a Hash and raised NoMethodError).
a = [1, 2, 3]
a = "s" if ARGV.size > 99
op = :+
p a.inject(10, op)
p a.reduce(10, op)
p a.inject(2, :*)
s = [10, :+]
p a.inject(*s)
def g(a, *s) = a.inject(*s)
g("s") if ARGV.size > 99
p g([1, 2, 3], 10, :+)
e = []
e = 2.5 if ARGV.size > 99
p e.inject(7, :+)
