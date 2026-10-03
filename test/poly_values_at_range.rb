# values_at on an Array read out of a boxed slot, with a Range among the
# indexes: the Range selects the run it covers, as on a typed Array. It
# read as the index 0, so `x.values_at(1..)` answered [1].

x = [[1, 2, 3], 1][0]
[(1..), (1..5), (-2..), (5..6), (5..), (3..), (2..0), (..1), (...1), (1...), (-3..-1), (0...-1)].each do |r|
  p x.values_at(r)
end
p x.values_at(0, 2..)
p x.values_at(1..2, 0, -1)
r = (1..)
p x.values_at(r)
p(begin; x.values_at(-5..1); rescue RangeError => e; e.message; end)

# beside a user class's own values_at, through the dispatch's default arm
class W; def values_at(*a) = :w; end
y = [[1, 2, 3], W.new][ARGV.size]
p y.values_at(1..)
p y.values_at(0, 2..)
p [[1, 2, 3], W.new][1].values_at(1..)

# a boxed Hash still looks its keys up
h = [{a: 1, b: 2}, 1][0]
p h.values_at(:b, :c)
