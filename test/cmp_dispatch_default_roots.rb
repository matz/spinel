# spinel: gc-stress
# A dispatch arm that fills a method's parameters from their defaults holds
# each default it builds in a rooted temp until the call: two allocating
# defaults (a Hash, an Array, a String, one reading self, two ahead of the
# operand) spelled inside the call were held by nothing while the next one
# allocated. Through the runtime cmp dispatch (sort_by, sort, min, max,
# min_by, Comparable on a boxed receiver) and the boxed binary operator.
# They run in source order, a scalar one that logs ahead of one that builds a
# value included, and a default that reads what a later one changes (the arm
# binds each to a local when two are filled and one has an effect), a
# keyword's default among them.
class V
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o, x = {}, y = {})
    x[:a] = n
    y[:b] = [n, o.n]
    x[:a] <=> y[:b][1]
  end
  def inspect = "V#{n}"
end
class K
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o, h = {k: n}, a = [n, o.n], s = "v#{n}")
    h[:k] + s.size * 0 <=> a[1]
  end
  def inspect = "K#{n}"
end
class L
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(x = [n], y = {m: [n]}, o) = x[0] + y[:m][0] - n <=> o.n
  def inspect = "L#{n}"
end
class C
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o, x = {}, y = [])
    x[:a] = n
    y << o.n
    x[:a] <=> y[0]
  end
  def inspect = "C#{n}"
end
class M
  attr_reader :n
  def initialize(n) = @n = n
  def +(o, x = {}, y = {})
    x[:a] = n
    y[:b] = o.n
    M.new(x[:a] + y[:b])
  end
  def inspect = "M#{n}"
end

$log = []
def la(x) = ($log << x; 1)
def lb(x) = ($log << x; [x])
class O
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o, a = la(:a), b = lb(:b)) = n <=> o.n + a * 0 + b.size * 0
  def inspect = "O#{n}"
end
class P
  attr_reader :n
  def initialize(n) = @n = n
  def +(o, a = la(:a), b = lb(:b)) = P.new(n + o.n + a + b.size)
  def inspect = "P#{n}"
end
$v = 1
$seen = []
def bump = ($v += 1; [0])
class Q
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o, a = $v, b = bump) = ($seen << [a, b]; n <=> o.n)
  def inspect = "Q#{n}"
end
class R
  attr_reader :n
  def initialize(n) = @n = n
  def +(o, a = $v, b = bump) = ($seen << [a, b]; R.new(n + o.n))
  def inspect = "R#{n}"
end
class S
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o, a = $v, b = [1, 2]) = ($seen << [a, b]; n <=> o.n)   # neither default runs code: nothing to order
  def inspect = "S#{n}"
end
class KP
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o, a = la(:a), k: lb(:k)) = n <=> o.n   # a positional default, then a keyword's
  def +(o, a = la(:a), k: lb(:k)) = KP.new(n + o.n + a + k.size)
  def inspect = "KP#{n}"
end
class KK
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o, j: la(:j), k: lb(:k)) = n <=> o.n   # two keyword defaults
  def +(o, j: la(:j), k: lb(:k)) = KK.new(n + o.n + j + k.size)
  def inspect = "KK#{n}"
end
class KE < StandardError
  def message(j: la(:j), k: lb(:k)) = "m#{j}#{k.size}"
end

data = (1..60).to_a.shuffle(random: Random.new(3))
w = ARGV.size > 5 ? 1 : data
f = proc { |x| V.new(x) }
p data.sort_by { |x| V.new(x) }.first(4), data.sort_by(&f).first(4), w.sort_by(&f).first(4), w.sort_by { |x| V.new(x) }.first(4)
vs = data.map { |x| V.new(x) }
bv = ARGV.size > 5 ? 1 : vs
p vs.sort.first(3), vs.min, vs.max, bv.sort.last, bv.min
p data.min_by { |x| V.new(x) }, data.max_by(&f)
p data.sort_by { |x| K.new(x) }.first(4), w.sort_by(&proc { |x| K.new(x) }).first(4)
p data.sort_by { |x| L.new(x) }.first(4), w.sort_by(&proc { |x| L.new(x) }).first(4)
a = ARGV.size > 5 ? 1 : C.new(1)
b = C.new(2)
p a < b, a <=> b, [b, a].sort, [a, b].max
s = ARGV.size > 5 ? 1 : M.new(0)
40.times { |i| s = s + M.new(i) }
p s

os = [O.new(2), O.new(1)]
p os.sort, $log
$log = []
p os.min, $log.each_slice(2).all? { |l| l == [:a, :b] }
$log = []
bo = ARGV.size > 5 ? 1 : O.new(1)
p bo < O.new(2), $log
$log = []
bp = ARGV.size > 5 ? 1 : P.new(1)
p bp + P.new(5), $log

$v = 1
p [Q.new(2), Q.new(1)].sort, $seen
$v = 1
$seen = []
bq = ARGV.size > 5 ? 1 : R.new(1)
p bq + R.new(2), $seen
$seen = []
p [S.new(2), S.new(1)].sort, $seen

$log = []
p [KP.new(2), KP.new(1)].sort, $log
$log = []
bk = ARGV.size > 5 ? 1 : KP.new(1)
p bk + KP.new(2), $log
$log = []
p [KK.new(2), KK.new(1)].sort, $log
$log = []
bj = ARGV.size > 5 ? 1 : KK.new(1)
p bj + KK.new(2), $log
$log = []
begin; raise KE; rescue => e; p e.message, $log; end
