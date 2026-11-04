# A global that only nil is written to reads nil wherever its value flows:
# a local, a parameter, an ivar or a method's return that also takes false,
# true, an Integer or a Float holds the nil as nil, not as false or 0.

$g1 = nil
x = $g1
p x
$g2 = false
x = $g2
p x

$a = nil
y = $a
p y.nil?
y = true
p y

$b = nil
$f = false
z = ARGV.empty? ? $b : $f
p z
[1, 2].each do |i|
  w = i == 1 ? $b : $f
  p w
end

$c = nil
k = $c
i = 0
while i < 2
  p k
  k = false
  i += 1
end

$d = nil
def show(v) = p(v)
show($d)
show(false)
show(5)

$e = nil
def pick(k) = k ? $e : 4
p pick(true)
p pick(false)

$h = nil
n = $h
p n
n = 5
p n + 1
fl = $h
p fl
fl = 1.5
p fl * 2

$q = nil
alias $r $q
s = $r
p s
s = false
p s

def reset = ($w = nil)
reset
t = $w
p t
t = true
p t

class Holder
  def initialize = (@v = $h)
  def bump = (@v = 3)
  def set(v) = (@x = v)
  def v = @v
  def x = @x
end
o = Holder.new
p o.v
o.bump
p o.v
o.set($g1)
p o.x
o.set(false)
p o.x

# a global whose other write is an empty Hash is not only nil: the Proc
# reading it answers that Hash
$e1 = nil
pr = proc { $e1 }
$e1 = {}
p pr.call
$e2 = nil
la = lambda { $e2 }
$e2 = Hash.new
h = la.call
p h.class, h.size
$e3 = nil
pr = proc { $e3 }
$e3 = {} if ARGV.empty?
p pr.call
$e4 = nil
pr = proc { $e4 }
$e4 ||= {}
p pr.call
