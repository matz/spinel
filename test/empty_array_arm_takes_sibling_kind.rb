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

# a sequence ending in an empty literal is a value of its own and keeps
# the general Array; an arm of statements ending in one is the literal
q1 = c ? (puts "seq"; []) : [1.5]
q1 << 2.5
q2 = [1] && (puts "and"; Array.new)
q2 << 4
q3 = if c
  puts "then"
  []
else
  [1.5]
end
q3 << 0.5
p q1, q2, q3

# an arm a static predicate folds to, of statements ending in `[]`, is
# built at the result's kind as the unfolded pair builds it
v = 7
q4 = if v.is_a?(Integer)
  puts "folded"
  []
else
  [1.5]
end
q5 = if v.is_a?(String) then [2.5] else puts "folded else"; [] end
q4 << 0.5
p q4, q5
