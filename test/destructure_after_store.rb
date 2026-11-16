# A multiple assignment from an Array held in a local takes the Array's
# elements as they are once every store into it is counted: a String, Integer
# or Float Array that a later store or push gives another kind of value hands
# its targets that value, as an indexed read of it does.
# spinel: gc-stress

# a String Array given an Integer (the shape of a parsed line)
tokens = "P 3 4".split(" ")
tokens[1..-1] = tokens[1..-1].map(&:to_i)
_, x, y = tokens
p x - 1, y * 2

t = ["a", "b"]
t[1] = 3
_, n = t
p n - 1

# a Float Array given a String; an Integer Array given a Symbol by a push
f = [1.5, 2.5]
f[1] = "s"
a, b = f
p a, b + "!"

ints = [1, 2]
ints << :three
_, _, c = ints
p c, c.to_s

# the splat target and a target after it
w = [1, 2]
w << "s"
h, *rest = w
*init, last = w
p h, rest, rest.last + "?", init, last * 2

# a target read again, and passed on
def show(v) = v.inspect
q = [7, 8]
q[0] = "q"
k, _ = q
copy = k
p copy + "!", show(k)

# in a method, and in a loop where only the last pass stores
def pair(n)
  r = [0.5] * n
  r[n - 1] = :sym
  u, v = r
  [u, v]
end
p pair(2)

sum = 0
4.times do |i|
  pr = [i, i * 2]
  pr[1] = "z" if i == 3
  lo, hi = pr
  sum += lo
  p hi
end
p sum

# a later multiple assignment in the loop replaces the Array: the first
# pass reads a Float, the second the String the replacement holds
src = [1.5, 2.5]
j = 0
while j < 2
  _, e = src
  src, unused = ["s", "q"], 0
  p e
  j += 1
end

# rows of a nested Array, one of them given a String
rows = [[1, 2], [3, 4]]
rows << [5, "x"]
rows.each { |row| _, s = row; p s }

# an Array no store widens keeps its kind
plain = [10, 20]
plain << 30
i1, i2, i3 = plain
p i1 + i2 + i3
