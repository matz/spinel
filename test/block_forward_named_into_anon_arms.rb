# The same forward through a named middle link, a poly receiver, and a
# top-level anonymous forwarder.
class Keeper
  def install(&h) = @h = h
  def fire(v) = @h.call(v)
end
class Keeper2
  def install(&h) = @h = h
  def fire(v) = @h.call(v + 1)
end
class Front
  def initialize(k) = @k = k
  def install(&) = @k.install(&)
  def fire(v) = @k.fire(v)
end
class Mid
  def initialize(k) = @f = Front.new(k)
  def install(&b) = @f.install(&b)
  def fire(v) = @f.fire(v)
end
class Outer
  def initialize(k) = @m = Mid.new(k)
  def install(&blk) = @m.install(&blk)
  def fire(v) = @m.fire(v)
end
o = Outer.new(Keeper.new)
o.install { |v| p v * 2 }
o.fire(5)
# a poly receiver in the anonymous forwarder
ks = [Keeper.new, Keeper2.new]
fs = ks.map { |k| Front.new(k) }
class Wrap
  def initialize(f) = @f = f
  def install(&blk) = @f.install(&blk)
end
fs.each { |f| Wrap.new(f).install { |v| p v * 10 } }
fs.each { |f| f.fire(1) }
# a top-level anonymous forwarder
$kk = Keeper.new
def tl_install(&) = $kk.install(&)
def tl_outer(&blk) = tl_install(&blk)
tl_outer { |v| p v + 100 }
$kk.fire(1)
