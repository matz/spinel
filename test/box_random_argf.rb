# A Random and ARGF in a boxed slot (an Array element, a Hash value, a
# mixed-kind local, a poly parameter) keep their identity: each read as nil.

r = Random.new(42)
x = [r, 1]
p x[0].class
p x[0].equal?(r)
h = {k: r}
p h[:k].class
y = rand > 2 ? 1 : r
p y.class
p y.nil?
def id(v) = v
p id(r).class
p id(1)
z = [ARGF, 2]
p z[0].class
p z[0]
w = [r, ARGF, "s"]
p w.map(&:class)
p [r, 1].inspect.start_with?("[#<Random")
GC.start
p w[0].class, w[1].class
