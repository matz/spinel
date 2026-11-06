# spinel: gc-stress
# The keys of a sort_by are compared with the key class's own <=>, which takes
# the other key whatever class that is: a <=> that answers nil for its own
# kind, beside a call that names it with an Integer, makes a pair of keys
# unordered (CRuby's ArgumentError), through a literal block, a forwarded proc
# and `&:k`. The operand has to hold the key's class, or the pair read as equal.
class NilSelf
  def <=>(o) = nil
end
class Ver
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = o.is_a?(Integer) ? n <=> o : o.n <=> n   # typed with an Integer, descending against its own kind
end
class W
  def initialize(k) = @k = k
  def k = @k
end
def t
  yield
rescue StandardError => e
  puts "#{e.class}: #{e.message}"
end

p(NilSelf.new <=> 1)
ns = NilSelf.new
g = proc { |i| ns }
t { p [0, 1].sort_by { |i| ns } }
t { p [0, 1].sort_by(&g) }
w = ARGV.size > 5 ? 1 : [0, 1]
t { p w.sort_by(&g) }
t { p w.sort_by { |i| ns } }

# `&:k` is a block answering the key too
t { p [W.new(ns), W.new(ns)].sort_by(&:k).size }
v = ARGV.size > 5 ? 1 : [W.new(ns), W.new(ns)]
t { p v.sort_by(&:k).size }

# a key that is a boxed Array holding a program object beside builtin values
data = ARGV.size > 5 ? [1] : [NilSelf.new, 2, NilSelf.new, "s"]
ks = data.map { |x| [x] }
idx = ARGV.size > 5 ? 1 : (0...ks.size).to_a
t { p idx.sort_by { |i| ks[i] }.size }
t { p idx.sort_by(&proc { |i| ks[i] }).size }
p(Ver.new(1) <=> 2)
vdata = ARGV.size > 5 ? [1] : [Ver.new(1), Ver.new(3), Ver.new(2)]
vks = vdata.map { |x| [x, "k"] }
vidx = ARGV.size > 5 ? 1 : (0...vks.size).to_a
p vidx.sort_by { |i| vks[i] }
p vidx.sort_by(&proc { |i| vks[i] })
