x = +"x"
a = [1, 2, 3]
a.fill(x)
x << "y"
p [a, a[0].equal?(a[1])]

# Mutation through a filled element must reach the original String as well.
y = +"z"
c = [1, 2]
c.fill(y)
c[0] << "!"
p [y, c]

# The indexed fill form stores the same handle too.
z = +"q"
d = [1, 2, 3]
d.fill(z, 1, 2)
z << "!"
p d
