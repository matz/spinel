# A class's own `method` is what `obj.method(:m)` calls (#10719): its super
# is Object#method, which builds the Method. `&obj.method(:m)` calls it once,
# not once per element, and a `method` that answers something else answers
# that.
class W
  def keep(x) = x * 2
  def method(n) = ($n += 1; super)
end
$n = 0
w = W.new
m = w.method(:keep)
p m.call(3), $n, m.name
p [1, 2].map(&w.method(:keep)), $n
p w.send(:method, :keep).call(5), $n

class V
  def keep(x) = x + 1
  def method(n) = ($v += 1; n == :other ? super(:keep) : super)
end
$v = 0
p V.new.method(:other).call(4), $v

class U
  def keep(x) = x
  def method(n) = :mine
end
p U.new.method(:keep)

# a class without one still builds the Method directly
class T
  def keep(x) = x * 3
end
p [1, 2].map(&T.new.method(:keep))
