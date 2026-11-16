# An Integer or Float that may be nil, and is nil, answers the methods nil
# shares with it as nil does: frozen? (true), dup / clone / freeze / itself
# (itself, nil), then / yield_self (the block's value for nil, `_1` and `it`
# too), and tap (itself) -- where the strict unwrap raised TypeError. An
# index answer of a typed Array is one, and a found one answers as a number.
ia = [3, 1]
m = ARGV.size == 0 ? 9 : 1
x = [1].tap(&:shift).first
y = [1.5].tap(&:shift).first
i = ia.index(m)
j = ia.index(1)
[x, y, i, j].each_with_index { |v, k| p [k, v.nil?] }
p x.frozen?, y.frozen?, i.frozen?, j.frozen?
p x.dup, y.clone, i.freeze, j.dup + 1, i.itself
p x.then { _1 }, y.yield_self { it }, i.then { |v| v.nil? ? :none : v }, j.then { |v| v * 10 }
p x.tap { }.then { _1 }, i.tap { |v| p v }
