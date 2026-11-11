# With one ** operand and a & operand that is a plain read, CRuby passes the Hash itself and converts the block first, so a key to_proc adds is passed; with a & operand that is a call, a safe-nav call or the implicit it, or with two ** operands, the Hash is copied first. One row for each shape.
class Adder
  def initialize(h) = @h = h
  def to_proc; @h[:x] = 1; proc {}; end
end
class Rec; def push(*a, **kw, &b) = [a, kw]; end
def run(l)
  h = {}; o = Adder.new(h); @o = o; $o = o
  r = yield(h, o)
  p [l, r]
end
run(:local)  { |h, o| [1].push(2, **h, &o) }
run(:ivar)   { |h, o| [1].push(2, **h, &@o) }
run(:gvar)   { |h, o| [1].push(2, **h, &$o) }
run(:new)    { |h, o| [1].push(2, **h, &Adder.new(h)) }
run(:user_local) { |h, o| Rec.new.push(1, **h, &o) }
run(:hcall_local) { |h, o| [1].push(2, **h.itself, &o) }
run(:two_kw) { |h, o| [1].push(2, **h, **{}, &o) }
class Tp
  def to_proc
    $h[:k] = 1
    proc { |*a| a }
  end
  def go = ($h = {}; [1].push(2, **$h, &self))
end
TP = Tp.new
module M; T = Tp.new; end
def via_param(t) = ($h = {}; [1].push(2, **$h, &t))
class Cv
  @@t = Tp.new
  def self.go = ($h = {}; [1].push(2, **$h, &@@t))
end
$h = {}; p [1].push(2, **$h, &TP)
$h = {}; p [1].push(2, **$h, &M::T)
p Tp.new.go
p via_param(Tp.new)
p Cv.go
[Tp.new].each { |bl| $h = {}; p [1].push(2, **$h, &bl) }
[Tp.new].each { $h = {}; p [1].push(2, **$h, &it) }
[Tp.new].each { $h = {}; p [1].push(2, **$h, &_1) }
[Tp.new].each { |x| bl = x; $h = {}; p [1].push(2, **$h, &bl) }
t = Tp.new
$h = {}; p [1].push(2, **$h, &t)
$h = {}; p [1].push(2, **$h, &t&.itself)
@t = Tp.new
$h = {}; p [1].push(2, **$h, &@t)
$t = Tp.new
$h = {}; p [1].push(2, **$h, &$t)
h = $h = {}; p [1].push(2, **h, &t)
h = $h = {}; p [1].push(2, **h, **{}, &t)
h = $h = {}; e = {}; p [1].push(2, **h, **e, &t)
