# spinel: gc-stress
# A key's own <=> with parameters beyond the operand is called with their
# defaults (one reading self and an earlier parameter, a keyword, a block
# slot), and one that needs a second argument raises CRuby's ArgumentError,
# through a literal block and a forwarded proc, typed and boxed.
class D
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o, s = 1) = (o.n <=> n) * s
end
class E
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o, s = n, t = [1, 2].size - 1) = (o.n <=> n) * s * t   # a default reading self and an earlier parameter
end
class F
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o, k: 1) = (o.n <=> n) * k
end
class G
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o, &b) = o.n <=> n
end
class H
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o, p) = 0
end
class J
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o, s = 1) = o.is_a?(Integer) ? n <=> o : (o.n <=> n) * s   # named with an Integer, ordered against its own kind
end

def t
  yield
rescue StandardError => e
  puts "#{e.class}: #{e.message}"
end
f = proc { |x| D.new(x) }
p [1, 3, 2].sort_by { |x| D.new(x) }
p [1, 3, 2].sort_by(&f)
w = ARGV.size > 5 ? 1 : [1, 3, 2]
p w.sort_by(&f)
p w.sort_by { |x| E.new(x) }
p [1, 3, 2].sort_by { |x| F.new(x) }
p [1, 3, 2].sort_by { |x| G.new(x) }
t { p [1, 3, 2].sort_by { |x| H.new(x) } }
p(J.new(1) <=> 2)
p [1, 3, 2].sort_by { |x| J.new(x) }
