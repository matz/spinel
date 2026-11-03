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
p((h(false) rescue :no_method))
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
