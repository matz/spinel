# A push through a boxed parameter stores each of its values: one of a kind
# the caller's Array cannot hold widens that Array where it is built, even
# beside values of other kinds.

def two(r, a) = r.push(a, "t")
def spl(r, *a) = r.push(*a, :s)
def kw(r, *a, **k) = r.push(*a, **k)
def lit(r, *a) = r.push(*a, {z: 1})
def front(r, a) = r.unshift(a, 2.5)
def mid(r, a) = r.insert(1, a, "m")
def outer(r, a) = two(r, a)

xs = [[1], +"s"]
p two(xs[0], 6)
p spl(xs[0], 7)
p kw(xs[0], 8, z: 1)
p kw(xs[0], 9)
p lit(xs[0], 10)
p front(xs[0], 0)
p mid(xs[0], 3)
p xs

ys = [[1], :y]
p outer(ys[0], 2)
p [two(ys[0], 3).size, spl(ys[0], 4).size]
n = kw(ys[0], 5, q: nil).last
p n
p ys

class Box
  def initialize = @rows = [[1], "r"]
  def fill = two(@rows[0], 4)
  def rows = @rows
end
b = Box.new
b.fill
p b.rows

# a value taken from another parameter, or from the rest, is this call's
# argument for it; so are another parameter's elements, splatted or concatenated
def rest(r, *a) = r.push(*a)
def outer(r, *a) = rest(r, *a)
def val(r, v) = r.push(v)
def spl(r, a) = r.push(*a)
def cat(r, v) = r.concat(v)
def ins(r, *a) = r.insert(1, *a)

zs = [[1], 2.5]
p outer(zs[0], 6, "t")
p val(zs[0], :u), val(zs[0], 7)
p spl(zs[0], [8, 9]), spl(zs[0], [10, "v"])
p cat(zs[0], ["w"]), cat(zs[0], [11])
p ins(zs[0], 1.5, nil), rest(zs[0], [])
p spl([1], [2, "x"]), cat([1], ["y"])
p zs

class Rows
  def initialize = @rows = [[1], "r"]
  def fill = rest(@rows[0], 6, "t")
  def rows = @rows
end
rw = Rows.new
rw.fill
p rw.rows

# a Hash keyed through the parameter takes its values, and the Array beside it
# stays as it is
def put(r, k, v)
  r[k] = v
  r
end
hs = [{a: 1}, [1]]
p put(hs[0], :b, "s"), put(hs[0], :c, 2), hs[1].push(3)
