# spinel: share
# A bang on a boxed receiver handed to a yield answers the receiver's box,
# which holds the String's handle, so a block that changes its parameter
# in place changes the String every holder of the box names: a local, an
# element, a Struct member, a global, through a block or a proc, bare or
# as an arm of a conditional.
def y(s) = yield(s)
def z
  s = [+"ab", 1].first
  yield(s.upcase!)
  p s
  yield(s.downcase!)
  p s
  yield(s.downcase!)
  p s
end
z { |x| x << "!" if x }
def w
  s = [+"cd", 1].first
  r = yield(s.upcase!)
  p s, r.equal?(s)
end
w { |x| x }
def v(&b)
  s = [+"ef", 1].first
  b.call(s.upcase!)
  p s
end
v { |x| x << "?" }

def y2
  s = [+"ab", 1].first
  t = +"cd"
  yield(s.size > 1 ? s.upcase! : t)
  p s
  t
end
y2 { |x| x << "!" if x }

def y3
  s = [+"ab", 1].first
  t = +"cd"
  yield(s.upcase! || t)
  p s
end
y3 { |x| x << "!" if x }

def z2(a)
  yield(a[0].upcase!)
  p a
end
z2([+"ab", 1]) { |x| x << "!" if x }

def y4(s) = yield(s.upcase!)
o = Struct.new(:v).new([+"ab", 1].first)
y4(o.v) { |x| x << "!" }
p o.v
def g = $g
$g = [+"cd", 1].first
y4($g) { |x| x << "?" }
p $g
