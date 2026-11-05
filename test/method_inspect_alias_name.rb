# A Method named apart from its def (an alias, alias_method, a chain of
# them, a class-side alias) writes the def's name after its own, as CRuby
# does: `C#bar(foo)(a, b=...)`. A Method of the def's own name does not.
def shown(m) = m.inspect.sub(/ \S+:\d+>\z/, ">")

class P; def foo = 1; end
class C < P
  def foo(a, b = 1) = a
  alias bar foo
  def to_s = "c"
  alias inspect to_s
  def x = 1
  alias_method :y, :x
  alias z y
  def ==(o) = true
  alias eql? ==
  def self.a = 1
  class << self; alias b a; end
end

o = C.new
puts shown(o.method(:bar)), shown(C.instance_method(:bar)), shown(o.method(:foo))
puts shown(o.method(:inspect)), shown(o.method(:z)), shown(o.method(:eql?))
puts shown(C.method(:b)), shown(C.method(:a))
puts shown(o.method(:bar).unbind), shown(C.instance_method(:y).bind(o))
m = o.method(:y)
m = o.method(:x) unless C.a > 1
puts shown(m)
puts shown([o.method(:y)][0])
p o.method(:z).original_name
