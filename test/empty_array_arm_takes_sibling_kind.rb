# An empty `[]` beside an Integer, Float or String Array is that Array
# An empty literal has no element type of its own. As the else arm of a
# conditional it took the other arm's Array kind, but as the then arm, or
# on the right of `||` / `&&`, it was joined as the general Array and boxed
# the whole expression: `c ? [] : [1, 2]` was boxed where `c ? [1, 2] : []`
# was an Integer Array. Each arm now takes the other's kind, and the value
# still widens where the program stores another kind into it.

c = ARGV.empty?
d = !c

x1 = c ? [] : [1, 2]
x2 = c ? [1, 2] : []
x3 = d ? [] : Array.new(2, 0)
x4 = [1, 2] || []
x5 = d ? [] : [1.5, 2.5]
x6 = d ? [] : %w[p q]
p x1.sum, x2.sum, x3.sum, x4.sum, x5.sum, x6.join

a = if d then [] else [1, 2] end
a << 3
b = unless c then [] else [4.5] end
b.push(1.25)
p a, b.sum

f = c ? [] : [1]
f << "s"
p f

g = [9] && []
g << 1
p g

def pick(c) = c ? [] : [1, 2, 3]
k = pick(true)
k << 10
m = pick(false)
m << "w"
p k, m

rows = Array.new(2) { |i| Array.new(2, i) }
r = rows.empty? ? [] : rows[1]
r[0] += 5
s = rows[5] || []
s << 1
p r, rows, s
