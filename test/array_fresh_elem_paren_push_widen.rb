# A push of another kind into a parenthesized new Array a builtin
# returns, held in an Array element or a Hash value, widens the Array.

n = [("ab".bytes)]
n[0] << "q"
p n

m = [("ab".bytes), (("cd".bytes))]
m[1].push("z", 3)
p m

h = { a: ("ab".bytes) }
h[:a] << "q"
p h

s = [("a b".split)]
s[0] << 1
p s

f = [([1.5, 2.5].map { |x| x * 2 })]
f.first << :x
p f
