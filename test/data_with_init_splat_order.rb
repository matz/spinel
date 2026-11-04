# Data#with on a Data with an initialize of its own: every keyword and
# `**hash` runs in source order after the receiver, and a keyword a later
# element names again still runs.
$log = []

Pt = Data.define(:x, :y) do
  def initialize(x: 0, y: 0)
    super(x: x, y: y)
  end
end

Tag = Data.define(:name, :n) do
  def initialize(name: "none", n: nil)
    super(name: name, n: n)
  end
end

def rcv(d)
  $log << :rcv
  d
end

def first_h
  $log << :first
  {x: 5}
end

def mark(v)
  $log << :"mark#{v}"
  v
end

def pick_h(h)
  h
end

pt = Pt.new(x: 1, y: 2)

r = rcv(pt).with(**first_h, **{y: mark(7)})
p r
p $log

$log = []
h = {y: 4}
r = pt.with(x: mark(9), **h, x: 3)
p r
p $log

$log = []
r = pt.with(x: mark(1), x: mark(2))
p r
p $log

# a value read before a later operand changes it
$v = 1
def bump
  $v += 10
  {y: 8}
end
r = pt.with(x: $v, **bump)
p r
p $v

# a later `**hash` wins over an earlier keyword; an earlier one loses
r = pt.with(x: mark(4), **pick_h({x: 6}))
p r
r = pt.with(**pick_h({x: 6}), x: mark(4))
p r

# a nullable member through `**`, from nil and to nil
tg = Tag.new(name: "a", n: 3)
p tg.with(**pick_h({n: nil}))
p tg.with(**{n: nil}, name: mark("b"))
p tg.with(n: nil).with(**pick_h({name: "c"}))
p tg.with(name: mark("d"), **pick_h({}))

