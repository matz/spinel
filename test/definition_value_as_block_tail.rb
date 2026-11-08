# spinel: int64
# A block whose last statement is a definition, when the block's value is used:
# `def` and `def self.x` answer their name as a Symbol; attr_reader, attr_writer
# and attr_accessor in a class body answer the Array of the names they define;
# `private def` answers the name. The block is given to a user method that
# yields (and returns or drops the value), to tap and each_with_object, to a
# proc called through &blk, and the definition is also a begin, ternary and
# method tail. Beside them a block that ends in a number whose value is used.
def run = yield
def drop = (yield; nil)
def keep
  r = yield
  [r, r.class]
end
def via_amp(&b) = b.call
def rec(n) = n == 0 ? yield : rec(n - 1) { yield }

x = run { def hi = :hi }
p x, hi
p run { def self.top_sing = 1 }
p run { run { def nested = 1 } }
p drop { def dropped = 1 }
p keep { def kept = 1 }
p via_amp { def amped = 1 }
p rec(2) { def recursed = 1 }
p [1, 2].tap { |a| def tapped = 1 }
p [1].each_with_object([]) { |v, acc| acc << v; def ewo = 1 }
p [hi, top_sing, nested, dropped, kept, amped, recursed, tapped, ewo]

big = 2**40
y = run { big += 1; def after_big = 1 }
p y, big

o = Object.new
p run { def o.single = :s }
p o.single
p({ run { def hkey = 1 } => 1 })
p [run { def e1 = 1 }, run { def e2 = 1 }]
p (run { def sym_of = 1 }).to_s

class A
  def foo = 1
  def self.run = yield
  a = run { attr_reader :q }
  p a
  p run { attr_accessor :r, :s }
  p run { attr_writer :t }
  p run { private def pv = 2 }
  p run { public def pc = 3 }
  p run { protected def pd = 4 }
  p [1].map { attr_reader :m }
  p [3].each_with_object([]) { |v, acc| acc << v; attr_accessor :n }
  p(begin attr_reader :u end)
  p(true ? attr_writer(:w) : 0)
  p alias_method(:bar, :foo)
  p private(:foo)
end

def mk
  def inner = :inner
end
p mk, inner
p (begin; def b1 = 1; end)
def pick(f) = f ? (def t_a = 1) : (def t_b = 1)
p pick(true), pick(false)
l = -> { def lam1 = 1 }
p l.call
p [1, 2].map { def mp = 1 }
p run { 5 }, run { :five }

# A visibility call of a def at top level, and a public or protected def that
# is called elsewhere, as a tail whose value is read.
r = run { private def tp = 3 }
p r
p tp
class V
  def self.run = yield
  r = run { public def pc = 3 }
  p r
  r = run { protected def pd = 4 }
  p r
  def cmp(o) = o.pd
end
p V.new.pc, V.new.cmp(V.new)
