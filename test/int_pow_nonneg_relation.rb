# An Integer power keeps the plain Integer path where the facts show its
# run-time exponent is not negative: `2 ** (max - depth)` under
# `depth <= max`, a sum or product of such, an abs, a container's size. Where
# they do not (the converse relation, a write between the test and the use, a
# closure writing a local, a program-defined operator), the power is a
# Rational for a negative exponent. Every power here prints its value, so a
# fact that is lost shows only as C, and one that is wrongly kept shows as a
# RangeError for the negative exponent; the exponents come from ARGV.size (0 at
# run time) so nothing folds.
# spinel: decisions

# the shape binary_trees has: the exponent is max - depth under depth <= max
max_depth = Integer(ARGV[0] || 6)
min_depth = 4
depth = min_depth
while depth <= max_depth
  p 2 ** (max_depth - depth + min_depth)
  depth = depth + 2
end

# the relation from each of < <= > >=, the loop conditions, until
d = ARGV.size
m = 3
while d < m
  p 2 ** (m - d)
  d += 1
end
d = ARGV.size
m = 2
while m >= d
  p 2 ** (m - d)
  d += 1
end
d = ARGV.size
m = 2
until d > m
  p 2 ** (m - d)
  d += 1
end

# the converse: d - m under d <= m is negative
d = ARGV.size + 1
m = 3
while d <= m
  p 2 ** (d - m)
  d += 1
end
d = ARGV.size + 5
m = 3
until d < m
  p 2 ** (m - d)
  d -= 1
end

# a write of either local between the test and the use
d = ARGV.size
m = 3
while d <= m
  d += 3
  p 2 ** (m - d)
end
d = ARGV.size
m = 2
while d <= m
  m -= 3
  p 2 ** (m - d)
  d += 1
end

# a closure writes the local between them
d = ARGV.size
m = 2
bump = -> { d += 5 }
while d <= m
  bump.call
  p 2 ** (m - d)
end

# an inner loop writes it after the use
a = ARGV.size
b = 2
while a <= b
  j = 0
  while j < 2
    p 2 ** (b - a)
    a += 3
    j += 1
  end
end

# the arms of an if / else and an unless / else
[[1, 3], [3, 1]].each do |x, y|
  if x <= y
    p 2 ** (y - x)
  else
    p 2 ** (y - x)
  end
end
a = ARGV.size + 4
b = 2
unless a > b
  p :no
else
  p 2 ** (b - a)
end

# a conjunction proves it, a disjunction does not
a = ARGV.size
b = 2
ok = ARGV.empty?
if a <= b && ok
  p 2 ** (b - a)
end
a = ARGV.size + 5
flag = ARGV.empty?
if a <= b || flag
  p 2 ** (b - a)
end

# sums and products of proven differences
a = ARGV.size
b = 0
while a <= b
  p 2 ** (b - a + 1)
  p 2 ** (b - a - 1)
  a += 1
end
a = ARGV.size
b = 2
k = ARGV.size - 1
if a <= b
  p 2 ** ((b - a) * 2)
  p 2 ** ((b - a) * k)
end

# an abs, a container's size
e = ARGV.size - 3
p 2 ** e.abs, 2 ** -e.abs
sz = [1, 2, 3, 4]
p 2 ** sz.size, 2 ** (sz.size - 5), 2 ** "abc".length

# a use inside a block in a loop, parameters, a Float operand, a nilable local
d = ARGV.size
m = 1
while d <= m
  [0].each { p 2 ** (m - d) }
  d += 1
end
def f(lo, hi)
  while lo <= hi
    p 2 ** (hi - lo)
    lo += 1
  end
end
f(ARGV.size, 2)
f(ARGV.size + 1, 1)
fm = 2.5 + ARGV.size
fd = 1
while fd <= fm
  p 2 ** (fm - fd)
  fd += 1
end
nm = ARGV.empty? ? 2 : nil
nd = 0
while nd <= nm
  p 2 ** (nm - nd)
  nd += 1
end

# a loop that runs once, and a use after a loop
d = ARGV.size
m = 2
begin
  p 2 ** (m - d)
  d += 1
end while d <= m
d = ARGV.size
m = 2
while d <= m
  d += 1
end
p 2 ** (m - d)
