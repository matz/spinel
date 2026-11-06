# spinel: gc-stress
# A key's <=> that answers a String (a literal, a computed one, a String
# subclass) is refused as CRuby's rb_cmpint refuses it, naming the answer's
# class, typed and through a forwarded proc on a boxed Array.
class S
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = (n - o.n).to_s
end
class MyS < String; end
class T
  def <=>(o) = MyS.new("x")
end
class U
  def <=>(o) = +"x"
end
def t
  yield
rescue StandardError => e
  puts "#{e.class}: #{e.message}"
end
t { p [3, 1, 2].sort_by { |x| S.new(x) } }
t { p [3, 1, 2].sort_by { |x| T.new } }
t { p [3, 1, 2].sort_by { |x| U.new } }
f = proc { |x| S.new(x) }
w = ARGV.size > 5 ? 1 : [3, 1, 2]
t { p w.sort_by(&f) }
