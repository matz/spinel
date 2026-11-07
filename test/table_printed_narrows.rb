# A table of Integer or Float rows handed whole to p, puts, print or pp
# keeps its rows unboxed: the printer reads it boxed by reference, with
# what its rows are. Every builtin argument had killed the table, so a
# program that printed its result grid at the end ran every step on it
# boxed, and a method handing back its table parameter to p boxed that
# parameter for all of its callers.

def bump(t)
  t.each { |r| r[0] += 100 }
  t
end
x = [[1, 2], [3, 4]]
p bump(x)

# each printer, one or several arguments
y = [[5, 6], [7]]
p y
puts y
print y, "\n"
pp y
p y, x
f = [[1.5], [2.25, 3.0]]
p f
puts f

# a method's table, printed straight or through a local
def build(n) = Array.new(n) { |i| Array.new(2) { |j| i * 10 + j } }
p build(3)
b = build(2)
p b
b[0] << 9
puts b

# an instance variable
class Grid
  def initialize(n)
    @rows = Array.new(n) { |i| [i, i * i] }
  end

  def show
    p @rows
    @rows.sum { |r| r.sum }
  end
end
p Grid.new(3).show

# an empty row nothing pushes into
e = [[1], []]
p e
puts e

# a row with a gap a write past its end left: puts prints the nil's
# empty line
gap = [[1], [2, 3]]
gap[0][2] = 5
puts gap
fgap = [[1.5], [2.5]]
fgap[0][2] = 3.5
puts fgap

# a method's fresh table of Float rows, held nowhere but by puts while
# each row renders (SPINEL_GC_STRESS)
def fresh = Array.new(30) { |i| [i * 1.5, i / 4.0] }
puts fresh

# p's value used: the table stays boxed, and is the same table
z = [[8, 9]]
w = p(z)
p w.equal?(z)

# an Array of objects stays boxed, and prints as before
class Err < StandardError; end
errs = [Err.new("one"), Err.new("two")]
p errs
class Pt
  def initialize(x) = (@x = x)
  def inspect = "Pt(#{@x})"
end
pts = [Pt.new(1), Pt.new(2)]
p pts
