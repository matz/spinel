# A block given to a method a TrueClass or FalseClass reopen defines.
class TrueClass
  def e = yield(self)
  def twice; yield 1; yield 2; :t; end
  def both(x = 0) = yield(x, :t)
  def opt = block_given? ? yield(1) : :none
  def bp(&b) = b.call(self)
end
class FalseClass
  def twice; yield 10; :f; end
  def both(x = 0) = yield(x, :f)
end
def flag(n) = n > 1

p true.e { |v| v }
p true.e { |v| [v, 1] }
pr = proc { |v| "got #{v}" }
p true.e(&pr)
p true.opt, true.opt { |v| v + 1 }
p true.bp { |v| [v] }

total = 0
r = flag(2).twice { |v| total += v }
p [r, total]
total = 0
r = flag(0).twice { |v| total += v }
p [r, total]

[true, false].each { |b| p b.both(3) { |x, s| [x, s] } }
p((flag(5).both { |x, s| break [:brk, s] }))
x = flag(1)
p x.both(1) { |a, b| a.to_s + b.to_s }

def g(v) = v.both { |x, s| [s, x] }
p g(true), g(false)
def h(v) = v.e { |x| [x] }
p h(true)
begin
  p h(false)
rescue NoMethodError
  p :no_method
end
begin
  false.e { |v| v }
rescue NoMethodError => err
  p err.message[0, 25]
end

# the two sides answer different types: the call's answer is boxed once
class TrueClass;  def bg = yield > 0; end
class FalseClass; def bg = :f; end
p true.bg { 1 }
p false.bg { 1 }
[true, false].each { |v| p v.bg { 2 } }

# a side's method that can answer nil: the call keeps the nil
class TrueClass;  def nn = yield ? 1 : nil;   def nf = yield ? 1.5 : nil; end
class FalseClass; def nn = yield ? 2 : nil;   def nf = yield ? 2.5 : nil; end
def nh(b, x) = b.nn { x }
def nk(b, x) = b.nf { x }
p nh(true, true), nh(true, false), nh(false, true), nh(false, false)
p nk(true, true), nk(true, false), nk(false, true), nk(false, false)
p true.nn { false }, false.nn { true }

# ... and boxed, when the sides answer different types
class TrueClass;  def nb = yield ? 1 : nil;   def nw = yield ? 1.5 : nil; end
class FalseClass; def nb = yield ? 2.5 : nil; def nw = yield ? 7 : 1; end
def bh(b, x) = b.nb { x }
def bj(b, x) = b.nw { x }
p bh(true, true), bh(true, false), bh(false, true), bh(false, false)
p bj(true, true), bj(true, false), bj(false, true), bj(false, false)
