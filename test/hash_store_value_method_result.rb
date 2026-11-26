# A method whose value is an element store (`node[name] = {...}`) returns
# the Hash it stored. The Hash comes back as the method's argument and takes
# a String key beside its Symbol keys.
def join(location, name)
  location[name] = {:name => name}
end
root = {}
a = join(root, "a")
b = join(a, "b")
p root.keys, a.keys, b.keys

# the stored Hash holds the Hash it was stored into
def nest(location, name)
  location[name] = {:name => name, :parent => location}
end
top = {}
c = nest(top, "c")
d = nest(c, "d")
p top.keys, c.keys, d.keys
p d[:parent].equal?(c)

# through a `return`, and from an instance method
def join_if(location, name)
  return location[name] = {:name => name} if name
  nil
end
e = join_if({}, "e")
f = join_if(e, "f")
p e.keys, f.keys, join_if(e, nil)

class Tree
  def initialize = @root = {}
  def add(node, name)
    node[name] = {:name => name, :kids => 0}
  end
  def root = @root
end
t = Tree.new
g = t.add(t.root, "g")
h = t.add(g, "h")
h[:kids] += 1
p t.root.keys, g.keys, h

# the same beside a method_missing of the program's: `x[k] = v` is v
class Ghost
  def method_missing(name, *args) = :ghost
  def respond_to_missing?(*) = true
end
p Ghost.new.anything
i = join({}, "i")
j = join(i, "j")
p i.keys, j.keys

# beside a `[]=` of the program's own that returns another value
class Box
  def []=(key, value)
    0
  end
end
p join(Box.new, "x")
k = join({}, "k")
l = join(k, "l")
p k.keys, l.keys

# beside a `[]=` that an alias binds: called by name it returns what the
# aliased method returns
class Sink
  def store(key, value)
    :stored
  end
  alias []= store
end
p Sink.new.[]=(:k, 1)
m = join({}, "m")
n = join(m, "n")
p m.keys, n.keys
