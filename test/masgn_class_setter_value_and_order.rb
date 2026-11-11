# A multiple assignment with a class's setter among its targets: CRuby reads
# every target's receiver and key first, left to right, then the values, then
# writes the targets left to right; and the assignment answers its right
# side. A target written ahead of the first setter, an effect in a key or
# receiver after it, and a read value were each wrong or refused.

$log = []
def k(x) = ($log << "k#{x}"; x)
def v(x) = ($log << "v#{x}"; x)
def o = ($log << "o"; O.new)
class K
  def self.a=(x); $log << "K.a=#{x}"; @a = x; end
  def self.a = @a
end
class H; def []=(i, x); $log << "H[#{i}]=#{x}"; end; end
class O; def x=(y); $log << "O.x=#{y}"; end; end
h = H.new
g = H.new

# keys and receivers run first, in order, then the values, then the stores
K.a, h[k(1)] = v(1), v(2)
p $log
$log = []
h[k(1)], K.a = v(1), v(2)
p $log
$log = []
K.a, o.x = v(1), v(2)
p $log
$log = []
h[k(1)], g[k(2)] = v(1), v(2)
p $log
$log = []
h[k(1)], K.a, h[k(2)] = v(1), v(2), v(3)
p $log

# the stores see each other's effects in order
class S
  def self.a=(x); @seen = @b; end
  def self.b=(x); @b = x; end
  def self.seen = @seen
end
S.b = :old
S.a, S.b = 1, :new
p S.seen

# a key or receiver read before the values keeps the value it had
class L; def self.a=(x); $a = x; end; end
m = {}
i = 0
L.a, m[i] = (i = 5; 1), 2
p $a, m, i
m = {}
n = {}
i = 0
m[(i = 5; :k)], L.a, n[i] = 1, 2, 3
p m, $a, n
n = {}
i = 0
L.a, n[i] = 1, (i = 9; 3)
p $a, n, i

# the assignment's value
x = (K.a, b = 1, 2)
p x, K.a, b
y = (b, K.a = [3, 4])
p y, K.a, b
arr = [5, 6]
z = (K.a, b = arr)
p z.equal?(arr), K.a, b
def tail = (K.a, b = 7, 8)
p tail
p((K.a, = 9, 10))
$log = []
r = (h[k(1)], K.a = v(1), v(2))
p r, $log
$log = []
r = (o.x, K.a = v(1), v(2))
p r, $log
if (K.a, b = nil, 2)
  p [:t, K.a, b]
end
w = (K.a, b = nil)
p w, K.a, b
p [1, 2].map { |j| (K.a, b = j, j * 2) }

# a to_ary object, a splat, a rescue
class T; def to_ary = [:p, :q]; end
t = T.new
x = (K.a, b = t)
p x.class, K.a, b
y = (K.a, *rest = 1, 2, 3)
p y, K.a, rest
class R; def self.a=(v); raise "no" if v == :bad; @a = v; end; def self.a = @a; end
def f(v) = (R.a, b = v, 1)
p f(:ok)
p((f(:bad) rescue $!.message))
p R.a
