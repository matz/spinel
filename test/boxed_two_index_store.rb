# A store with two indices (`c[y, x] = v`, `c.[]=(y, x, v)`) through a box
# that can hold an instance of a program class whose `[]=` takes them calls
# that `[]=`; a real Array or String in the same box still splices.
# spinel: gc-stress

class Grid
  def initialize = @h = {}
  def [](y, x) = @h[[y, x]]
  def []=(y, x, v)
    @h[[y, x]] = v
  end
end

class Cell
  def [](y) = 0
  def []=(y, v)
    v
  end
end

boxes = [Grid.new, Cell.new, [1, 2, 3, 4], +"abcdef"]
g = boxes[0]
g[3, 3] = 9
p g[3, 3]
p(g[1, 1] = 5)
g.[]=(4, 4, 7)
p g[4, 4], g[1, 1]

# the box's Array and String take the builtin splice
a = boxes[2]
a[1, 2] = [9]
p a
s = boxes[3]
s[0, 3] = "Z"
p s

# through an element read, and as a counter in a method
held = [g]
held[0][2, 2] = "two"
p held[0][2, 2]

def bump(m, y, x)
  m[y, x] = (m[y, x] || 0) + 1
end
3.times { bump(boxes[0], 0, 0) }
p g[0, 0]

# a receiver read from a table whose outer or index runs code: each runs
# once, and the splice is stored back into the row the first run chose
rows = [[1, 2], [3, 4], [5, 6]]
rb = [rows, Grid.new][0]
i = 0
rb[i += 1][0, 1] = [9]
p rows, i
$calls = 0
def rows_of(t)
  $calls += 1
  t
end
rows_of(rb)[2][1, 1] = [8]
p rows, $calls
strs = [+"abc", +"def"]
sb = [strs, Grid.new][0]
j = -1
sb[j += 1][0, 1] = "Z"
p strs, j
# the outer a Hash, and a program object, whose element is the class's
gh = Grid.new
by_key = { 0 => gh, 1 => [1, 2] }
k = -1
by_key[k += 1][0, 1] = 9
p gh[0, 1], k
class Shelf
  def initialize(a) = @a = a
  def [](n) = @a[n]
end
gs = Grid.new
shelf = Shelf.new([gs, [1, 2]])
m = -1
shelf[m += 1][1, 0] = 4
p gs[1, 0], m
# an outer held inline (a value-type object) whose `[]` answers the class's
class InlineShelf
  def initialize = @n = 0
  def [](n) = $inline_rows[n]
end
gi = Grid.new
$inline_rows = [gi, [1, 2]]
inline = InlineShelf.new
q = -1
inline[q += 1][0, 1] = 9
p gi[0, 1], q
# and one a method call answers, held by value
def made_inline = InlineShelf.new
gm = Grid.new
$inline_rows[0] = gm
q = -1
made_inline[q += 1][1, 0] = 2
p gm[1, 0], q
# an index that assigns the outer runs after it, as in Ruby: the store
# reaches the element of the outer read first
class BaseShelf
  def initialize(b) = @b = b
  def [](n) = $based_rows[@b + n]
end
class TagShelf
  def initialize(b) = (@tag = "t"; @b = b)
  def [](n) = $based_rows[@b + n]
  def tag = @tag
end
g0 = Grid.new
g1 = Grid.new
$based_rows = [g0, g1]
bs = BaseShelf.new(0)
bo = BaseShelf.new(1)
bs[(bs = bo; 0)][0, 1] = 9
ts = TagShelf.new(0)
to = TagShelf.new(1)
ts[(ts = to; 0)][1, 1] = 8
p g0[0, 1], g1[0, 1], g0[1, 1], g1[1, 1], ts.tag

# an outer call that changes what an index built above the statement reads
# (`[$k][0]`) runs first, as in Ruby
$krows = [Grid.new, Grid.new, [1, 2]]
$kk = 0
def krows
  $kk = 1
  $krows
end
krows[[$kk][0]][0, 1] = 9
p $krows[0][0, 1], $krows[1][0, 1]

# a value that runs code: the outer and the index run once, before it, and
# the store reaches the element they chose
$olog = []
$orows = [Grid.new, Grid.new, [1, 2, 3], [4, 5, 6], [10, 8, 9]]
$ok = 2
def orows_call
  $olog << :o
  $ok += 1
  $orows
end
def oval_call
  $olog << :v
  $ok += 1
  7
end
oi = $ok
local_rows = $orows
local_rows[(oi += 1)][0, 1] = oval_call
p $olog, $orows[3], oi
$olog.clear
orows_call[$ok][0, 1] = oval_call
p $olog, $orows.map { |r| r.is_a?(Grid) ? r[0, 1] : r }

# a value that puts another element in the outer's slot: the store reaches
# the element read before it, as in Ruby
eold = Grid.new
enew = Grid.new
erows = [eold, [1, 2]]
def ekey = 0
erows[ekey][0, 1] = (erows[0] = enew; 6)
p eold[0, 1], enew[0, 1]

# a store whose class takes one index fewer raises, as in Ruby
c = boxes[1]
begin
  c[1, 2] = 3
rescue ArgumentError => e
  p e.class
end
