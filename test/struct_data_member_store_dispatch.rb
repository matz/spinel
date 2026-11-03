# A value stored into a Struct's or Data's member by the builtin
# initialize (through super, a splat or a `**`) or by Struct#[]= is read
# back through the member in another method, which calls the element's
# method.

# super in a Data's initialize maps the keywords to other members
Pt = Data.define(:x)
class Pt
  def fade = "pt #{x}"
end
Pair = Data.define(:a, :b)
class Pair
  def initialize(a:, b:) = super(a: b, b: a)
  def paint = b.each { |pt| puts pt.fade }
end

Pair.new(a: [Pt.new(1)], b: []).paint

# super in a Struct's initialize builds a member
Dot = Struct.new(:x)
class Dot
  def label = "dot #{x}"
end
class Grid < Struct.new(:live, :n)
  def initialize(n) = super([Dot.new(n)], n)
  def paint = live.each { |d| puts d.label }
end

Grid.new(2).paint

# a splat and a `**` hand on what they hold
Bit = Data.define(:x)
class Bit
  def flip = "bit #{x}"
end
Byte = Data.define(:bits)
class Byte
  def paint = bits.each { |b| puts b.flip }
end

args = [[Bit.new(3)]]
Byte.new(*args).paint
Nib = Data.define(:x)
class Nib
  def flip = "nib #{x}"
end
Word = Data.define(:nibs)
class Word
  def paint = nibs.each { |n| puts n.flip }
end

kw = {nibs: [Nib.new(4)]}
Word.new(**kw).paint

# a `**` into a method's keyword
Leaf = Data.define(:x)
class Leaf
  def rustle = "leaf #{x}"
end
def rake(leaves:) = leaves.each { |l| puts l.rustle }
pile = {leaves: [Leaf.new(5)]}
rake(**pile)

# Struct#[]= by a Symbol, a String, a negative index and ||=
Jewel = Struct.new(:x)
class Jewel
  def cut = "gem #{x}"
  def polish = "polished #{x}"
  def set = "set #{x}"
  def mount = "mounted #{x}"
end
Case = Struct.new(:n, :row, :tray, :box, :bag)
class Case
  def show_row = row.each { |g| puts g.cut }
  def show_tray = tray.each { |g| puts g.polish }
  def show_box = box.each { |g| puts g.set }
  def show_bag = bag.each { |g| puts g.mount }
end

c = Case.new(0, [], [], [], nil)
c[:row] = [Jewel.new(6)]
c["tray"] = [Jewel.new(7)]
c[-2] = [Jewel.new(8)]
c[:bag] ||= [Jewel.new(9)]
c.show_row
c.show_tray
c.show_box
c.show_bag

# Struct#[]= by a computed key
Coin = Struct.new(:x)
class Coin
  def toss = "coin #{x}"
end
Purse = Struct.new(:n, :coins)
class Purse
  def show = coins.each { |co| puts co.toss }
end

pu = Purse.new(0, [])
key = ARGV.empty? ? :coins : :n
pu[key] = [Coin.new(10)]
pu.show

# a positional argument after a splat, and keywords as a positional Hash
Bead = Data.define(:x)
class Bead
  def glint = "bead #{x}"
  def roll = "rolled #{x}"
  def drop = "dropped #{x}"
end
Thread2 = Struct.new(:a, :b)
class Thread2
  def show_b = b.each { |bd| puts bd.glint }
  def show_hash = b.each_value { |bd| puts bd.roll }
end
head = [nil]
Thread2.new(*head, [Bead.new(11)]).show_b
Thread2.new(0, b: Bead.new(12)).show_hash
Loose = Struct.new(:opts, :n, keyword_init: false)
class Loose
  def show = opts.each_value { |bd| puts bd.drop }
end
Loose.new(opts: Bead.new(13)).show
