# spinel: gc-minor
# spinel: gc-stress
# An element write keeps its Array alive while its operands run code the program does not name:
# an index's to_int, an interpolation's to_s, a case's ===, and a proc that rebinds a captured
# local. The receiver is an ivar or a closure's local; a plain local no operand writes needs no root.
def churn(n)
  k = 0
  n.times { |i| k += ("y" * 50 + i.to_s).size }
  k
end

class Box
  attr_accessor :a, :s

  def initialize
    @a = [1, 2, 3]
    @s = [+"p", +"q"]
  end

  def and_int(j)
    t = 0
    @a[j] &&= (t = 5; 9)
    [t, @a]
  end

  def and_str(j)
    t = 0
    @s[j] &&= (t = 5; "zz")
    [t, @s]
  end

  def store(j)
    @a[j] = 9
    @a
  end

  def val_and(o)
    x = (@s[1] &&= "<#{o}>")
    [x, @s]
  end

  def stmt_interp(o)
    t = 0
    @s[1] &&= (t = 5; "<#{o}>")
    [t, @s]
  end

  def stmt_case(o)
    t = 0
    @a[1] &&= (t = 5; case 3 when o then 9 else 8 end)
    [t, @a]
  end
end

class Ix
  def initialize(b)
    @b = b
  end

  def to_int
    @b.a = [0]
    @b.s = [+"o"]
    churn(3000)
    1
  end
end

class Dr
  def initialize(b)
    @b = b
  end

  def to_s
    @b.a = [0]
    @b.s = [+"o"]
    churn(3000)
    "D"
  end

  def ===(x)
    to_s
    true
  end
end

bx = Box.new
p bx.and_int(Ix.new(bx))
bx = Box.new
p bx.and_str(Ix.new(bx))
bx = Box.new
p bx.store(Ix.new(bx))
bx = Box.new
bx.a[Ix.new(bx)] = 9
p bx.a
bx = Box.new
p bx.val_and(Dr.new(bx))
bx = Box.new
p bx.stmt_interp(Dr.new(bx))
bx = Box.new
p bx.stmt_case(Dr.new(bx))

class Cell
  def initialize(pr)
    @pr = pr
  end

  def to_s
    @pr.call
    churn(3000)
    "D"
  end
end

def run
  arr = [+"p", +"q"]
  rb = -> { arr = [+"o"] }
  o = Cell.new(rb)
  t = 0
  arr[1] &&= (t = 5; "<#{o}>")
  [t, arr]
end
p run
