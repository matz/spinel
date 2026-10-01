# A String variable a yield or instance_exec hands on through a splat -- an
# element of a splatted Array, or an argument written beside the splat --
# is the caller's own String, as in CRuby: the block parameter it lands on
# appends to it. The binding was refused at compile time, or appended to a
# copy where the String reached a rest or a post (#6179).

X = "x" * 40

# a method's parameter in an inline Array literal, and in a local Array
def y1(v) = yield(*[v])
s = +"a"; y1(s) { |a| a << X }; p s.size
def y2(v) = (r = [v]; yield(*r))
s = +"a"; y2(s) { |a| a << X }; p s.size

# a value written before the splat, and after an empty one
def y3(v) = yield(v, *[1])
s = +"a"; y3(s) { |a, b| a << X }; p s.size
def y4 = (v = +"a"; yield(*[], 1, v); p v.size)
y4 { |a, b| b << X }

# a local Array the yield spreads beside a value, into a rest and a post
def y5 = (v = +"a"; s = [v]; yield(*s, 2); p v.size)
y5 { |a, *r, b| a << X }
def y6(v) = yield(1, *[v])
s = +"a"; y6(s) { |n, *r| r[0] << X }; p s.size
def y7(v, w) = yield(w, *[v])
s = +"a"; t = +"b"; y7(s, t) { |a, *r| r[0] << X; a << X }; p s.size, t.size

# a local Array the method grows first, into two parameters
def y8 = (v = +"a"; s = [v]; s << +"t"; yield(*s); p v.size)
y8 { |a, b| a << X; b << X }

# the same block yielded to with and without a splat, a keyword beside it,
# a method that keeps its block, and a proc passed with `&`
def y9(v) = (yield(*[v]); yield(v))
s = +"a"; y9(s) { |a| a << X }; p s.size
def y10(v) = yield(*[v], k: 1)
s = +"a"; y10(s) { |a, k:| a << X * k }; p s.size
def y11(v, &b) = (@b = b; yield(*[v]))
s = +"a"; y11(s) { |a| a << X }; p s.size
def y12(v) = yield(*[v])
pr = proc { |a| a << X }
s = +"a"; y12(s, &pr); p s.size

# a parameter another kind widens to a box, and a block handing its
# parameter to a method that appends to it
def y13(v) = yield(*[v])
s = +"a"; y13(s) { |a| a << X }; p s.size
y13(+"q") { |b| p b }
y13(5) { |c| p c }
def gr(v) = v << X
gr([])
def y14(v) = yield(*[v])
s = +"a"; y14(s) { |a| gr(a) }; p s.size

# an iterator method, and a frozen String
class K; def each_pair(v) = yield(*[v, 1]); end
s = +"a"; K.new.each_pair(s) { |a, i| a << X * i }; p s.size
fz = "fr".freeze
begin; y1(fz) { |a| a << X }; rescue FrozenError => e; p e.class; end

# instance_exec with a splat of a local Array, into a rest, and a block
# handing the parameter on
o = Object.new
v = +"a"; s = [v]; o.instance_exec(*s, 2) { |a, b| a << X }; p v.size
v = +"a"; w = +"w"; s = [v, w]; o.instance_exec(*s) { |a, *r| a << X; r[0] << X }; p v.size, w.size
v = +"a"; s = [v]; o.instance_exec(*s, 2) { |a, b| gr(a) }; p v.size
