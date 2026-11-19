# A runtime-name send answers the String its target answers, not a copy of
# it: send, public_send and __send__ alike. A target that reads an instance
# variable (through a method of its own, an attr_reader or an
# attr_accessor), hands a parameter back, hands back the String it replaced,
# or answers nil or a value of another class: the answer kept, changed or
# compared with equal? is the target's own String.
class Holder
  def initialize(s) = @s = s
  def peek = @s
  def echo(x) = x
  def size = @s.size
end

class Cell
  def initialize(s) = @s = s
  def peek = @s
  def maybe(flag) = flag ? @s : nil
  def either(flag) = flag ? @s : 7
  def swap(x) = (old = @s; @s = x; old)
end

class Fields
  attr_reader :s
  attr_accessor :w
  def initialize(s) = (@s = s; @w = s)
  def size = @s.size
end

h = Holder.new(+"iv")
names = [:peek, :echo, :size]

# a method reading the instance variable
r = h.send(names[0]); r << "1"; p [h.peek, r.equal?(h.peek)]
r = h.public_send(names[0]); r << "2"; p [h.peek, r.equal?(h.peek)]
r = h.__send__(names[0]); r << "3"; p [h.peek, r.equal?(h.peek)]
k = h.send(names[0]); p k.equal?(h.peek)

# an attr_reader and an attr_accessor
f = Fields.new(+"fd")
fn = [:s, :w, :size]
r = f.send(fn[0]); r << "4"; p [f.s, r.equal?(f.s)]
r = f.public_send(fn[1]); r << "5"; p [f.s, f.w, r.equal?(f.w)]
r = f.__send__(fn[0]); r << "6"; p [f.s, r.equal?(f.s)]

# a parameter
s = +"par"
r = h.send(names[1], s); r << "1"; p [s, r.equal?(s)]
r = h.public_send(names[1], s); r << "2"; p [s, r.equal?(s)]
r = h.__send__(names[1], s); r << "3"; p [s, r.equal?(s)]

# the name read from a String
y = h.send(names[0].to_s); y << "7"; p h.peek

# nil, another class, and the String a method replaced
g = Cell.new(+"cl")
n = g.send([:maybe, :peek].first, false); p n
m = g.send([:maybe, :peek].first, true); m << "8"; p g.peek
e = g.send([:either, :peek].first, true); e << "9"; p g.peek
e = g.send([:either, :peek].first, false); p e
o = g.send([:swap, :peek].first, +"new"); o << "!"; p [o, g.peek]

# the answer put in a container or a variable, then changed
c = Holder.new(+"k")
a = [c.send([:peek, :size].first), 1]
a[0] << "a"
p c.peek
hh = { s: c.send([:peek, :size].first) }
hh[:s] << "h"
p c.peek
q = c.send([:peek, :size].first)
q << "q"
p [c.peek, q.equal?(c.peek), q.frozen?]

# compared with the String it is
z = +"zz"
d = Holder.new(z)
p d.send([:peek, :size].first).equal?(z)
p d.send([:peek, :size].first).equal?(d.peek)

# a receiver the send evaluates once
class Box
  attr_reader :n
  def initialize(h) = (@h = h; @n = 0)
  def next_holder = (@n += 1; @h)
end
b = Box.new(c)
w = b.next_holder.send([:peek, :size].first); w << "5"; p [c.peek, b.n]

# a target answering a new String or a frozen literal
class Maker
  def fresh = +"new"
  def lit = "lit"
  def size = 3
end
mk = Maker.new
mn = [:fresh, :lit, :size]
r = mk.send(mn[0]); r << "!"; p r
a = mk.send(mn[0])
b = a
a << "x"
p [a, b, a.equal?(b)]
r = mk.send(mn[1])
r2 = mk.send(mn[1])
p [r, r.frozen?, r.equal?(r2)]
begin
  r << "x"
rescue FrozenError => e
  p e.class
end

# a builtin answering its receiver
t = +"abc"
bn = [:to_s, :itself, :+@, :upcase!, :size]
u = t.send(bn[0]); u << "1"; p [t, u.equal?(t)]
u = t.public_send(bn[1]); u << "2"; p [t, u.equal?(t)]
u = t.__send__(bn[2]); u << "3"; p [t, u.equal?(t)]
u = t.send(bn[3]); u << "4"; p [t, u.equal?(t)]
p t.send(bn[4])

# a builtin answering a new String leaves the receiver alone
v = t.send([:capitalize, :size].first); v << "!"; p [t, v]

# a boxed receiver
objs = [h, 1]
x = objs.first.send(names[0]); x << "6"; p h.peek

# a reader of a boxed receiver, and capitalize of an empty String
fl = Fields.new(+"bx")
fobjs = [fl, 1]
fx = fobjs.first.send(fn[0]); fx << "7"; p fl.s
e0 = +""
e1 = e0.send([:capitalize, :size].first); e1 << "x"; p [e0, e1]

case_method = [:capitalize, :upcase].first
ascii_empty = +""
ascii_result = ascii_empty.send(case_method, :ascii)
ascii_result << "x"
p [ascii_empty, ascii_result.equal?(ascii_empty)]
binary_empty = "".b
binary_result = binary_empty.send(case_method, :ascii)
binary_result << "x"
p [binary_empty, binary_result.equal?(binary_empty), binary_result.encoding.name]
nul_string = +"\0a"
nul_result = nul_string.send(case_method, :ascii)
nul_result << "x"
p [nul_string.bytes, nul_result.bytes, nul_result.equal?(nul_string)]

# a receiver that runs code is evaluated once, ahead of the name and the
# arguments, and the arm that answers its String answers that String
$gn = 0
$glog = []
$gs = +"abc"
$gt = +"xyz"
def gs = ($gn += 1; $glog << "recv"; $gs)
def gpick(f) = f ? $gs : $gt
def garg = ($glog << "arg"; 1)
def gname = ($glog << "name"; :size)
gm = [:to_s, :itself, :+@, :upcase!, :size]
u = gs.send(gm[0]); u << "1"; p [$gs, u.equal?($gs), $gn]
u = gs.public_send(gm[1]); u << "2"; p [$gs, u.equal?($gs), $gn]
u = gs.__send__(gm[2]); u << "3"; p [$gs, u.equal?($gs), $gn]
u = gs.send(gm[3]); u << "4"; p [$gs, u.equal?($gs), $gn]
p gs.send(gm[4])
u = (1; $gs).send(gm[0]); u << "5"; p [$gs, u.equal?($gs)]
u = gpick(true).send(gm[0]); u << "6"; p [$gs, $gt, u.equal?($gs)]
u = gpick(false).send(gm[0]); u << "7"; p [$gs, $gt, u.equal?($gt)]
u = ($gn > 0 ? $gs : $gt).send(gm[0]); u << "8"; p [$gs, u.equal?($gs)]
$glog.clear
gs.send(gname)
p $glog
$glog.clear
class Acc
  def initialize = @s = +"acc"
  def add(x) = (@s << x.to_s; @s)
  def peek = @s
end
ac = Acc.new
def gacc(a) = ($glog << "recv"; a)
u = gacc(ac).send([:add, :peek].first, garg); u << "!"; p [ac.peek, $glog, u.equal?(ac.peek)]
def gnum = 5
p gnum.send([:to_s, :succ].last)
def garr = [1, 2]
p garr.send([:first, :size].first)
def gpoly = [$gs, 1].first
u = gpoly.send(gm[0]); u << "9"; p [$gs, u.equal?($gs)]
hh2 = { "a" => +"x" }
u = hh2["a"].send(gm[0]); u << "!"; p [hh2["a"], u.equal?(hh2["a"])]
ar2 = [+"y"]
u = ar2.first.send(gm[0]); u << "?"; p [ar2, u.equal?(ar2[0])]

# a receiver the name or an argument rebinds is the String it was when it
# was evaluated, ahead of them
rs = +"one"
rt = +"two"
rv = rs
u = rv.send((rv = rt; [:itself, :size].first)); u << "?"; p [rs, rt]
rv = rs
u = rv.__send__((rv = rt; [:to_s, :size].first)); u << "!"; p [rs, rt]
class Rebind
  def initialize = (@a = +"a"; @b = +"b"; @v = @a)
  def go = (u = @v.send((@v = @b; [:itself, :size].first)); u << "!"; [@a, @b])
end
p Rebind.new.go
$ra = +"a"
$rb = +"b"
$rv = $ra
def rswap = ($rv = $rb; :to_s)
u = $rv.send(rswap); u << "!"; p [$ra, $rb]

# a receiver a lambda the name calls rebinds is the String it was
ls2 = +"one"
lt2 = +"two"
lv2 = ls2
sw = -> { lv2 = lt2 }
u = lv2.send((sw.call; [:itself, :size].first))
u << "!"
p ls2, lt2
