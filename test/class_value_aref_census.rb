# An Array or Hash subclass that only a Class value's `k[..]` builds is in
# the census of classes that can exist from the start, as one built by `.new`
# is: an op-write on a box holding one runs its own `[]` and `[]=`. And an
# empty `[]` or `{}` handed to such a constructor is a container element of
# the class's Array, beside the Integers its other constructions hold.
# (No `.new` on a Class value, nor a `[]` on a local the compiler knows holds
# one class, both of which keep every class in the census:
# class_value_new_empty_elem.rb has those.)

def pick(i) = i == 0 ? MyA : Array
def pickh(i) = i == 0 ? MyH : Hash

class MyA < Array
  def [](i)
    puts "MyA#[] #{i}"
    super
  end

  def []=(i, v)
    puts "MyA#[]= #{i} #{v}"
    super
  end
end

class MyH < Hash
  def [](k)
    puts "MyH#[] #{k}"
    super
  end

  def []=(k, v)
    puts "MyH#[]= #{k} #{v}"
    super
  end
end

class Foo
  def tag = 1
end

# `pick(0)[1, 2]` is MyA's only construction
o = pick(ARGV.size)[1, 2]
b = [Foo.new, o][ARGV.size + 1]
b[0] += 10
p b, b.class

# a Hash subclass the same way
h = pickh(ARGV.size)[[[:a, 1]]]
g = [Foo.new, h][ARGV.size + 1]
g[:a] += 5
p g, g.class

# a subclass of the subclass
class MyB < MyA; end
def pickb(i) = i == 0 ? MyB : Array
s = [Foo.new, pickb(ARGV.size)[3, 4]][ARGV.size + 1]
s[1] += 1
p s, s.class

# a constant holding the Class value
class MyK < MyA; end
def pickk(i) = i == 0 ? MyK : Array
K = pickk(ARGV.size)
q = [Foo.new, K[5, 6]][ARGV.size + 1]
q[0] += 1
p q, q.class

# an empty container element, beside the Integers MyC holds elsewhere
class MyC < Array; end
x = MyC.new
x << 1 << 2
p x.sum
def pickc(i) = i == 0 ? MyC : Array
k = pickc(ARGV.size)
e = k[[]]
p e, e.class
f = k[{}]
p f, f.first.class
p k[[], {}]

# a nil element, through `k[..]` and `k.new(n, v)`, beside the Integers MyC holds
n = k[nil, 1]
p n, n.class, n[0], n[1]
m = k.new(2, nil)
p m, m.class, m[0]
m[0] = 5
p m, m.size
