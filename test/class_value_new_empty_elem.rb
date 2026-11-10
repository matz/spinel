# An empty `[]` or `{}` handed to a Class value's `new(n, v)` or `[..]` is a
# container element of the Array subclass it builds, beside the Integers the
# class's other constructions hold: the elements stay the containers they
# are, and one `new(n, [])` shares its one element.
class MyA < Array; end
x = MyA.new
x << 1 << 2
p x.sum
def pick(i) = i == 0 ? MyA : Array

k = pick(ARGV.size)
a = k.new(1, [])
p a, a.class, a.first.class
w = k.new(2, [])
w[0] << 5
p w
p k.new(1, {})
e = k[[]]
p e, e.first.class

# the same on a Class value the compiler knows (its own class: its static
# constructions are evidence for MyS)
class MyS < Array; end
y = MyS.new
y << 1
j = MyS
p j.new(1, []), j.new(2, {}).class, j[[]], j[{}]
