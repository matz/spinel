# a respond_to? super keeps its switch over the names the class answers;
# a Symbol or a String made at run time is told apart from them by its
# name: one that names no method answers false, one that names a method
# answers for it
class A
  def respond_to?(*n) = super
  def foo; end
  private def priv; end
end
class B < A
  def bar; end
end
class C < B
  def respond_to?(m, *r) = super
end
names = [:foo, "foo", "zz#{1}".to_sym, "f#{"oo"}".to_sym, "bar", :priv, "priv", :puts, "hash", :zork, "s"]
[A.new, B.new, C.new].each do |o|
  p names.map { |m| o.respond_to?(m) }
  p names.map { |m| o.respond_to?(m, true) }
end
v = [1, :foo].last
w = [1, "bar"].last
p A.new.respond_to?(v), B.new.respond_to?(w), C.new.respond_to?(w, true)
p [A.new.respond_to?("zz#{2}".to_sym), A.new.respond_to?(:foo)]
r = [1].map { C.new.respond_to?("pr#{"iv"}", true) }
p r
def t(o) = o.respond_to?("ba#{"r"}".to_sym)
p t(B.new), t(A.new)

# an Exception or Struct subclass, a plain count
class E < StandardError
  def respond_to?(m, a = false) = super
  def foo; end
end
S = Struct.new(:x) do
  def respond_to?(m, a = false) = super
end
e = E.new
p e.respond_to?("zz#{1}".to_sym), e.respond_to?(:foo), e.respond_to?("s"), e.respond_to?("fo#{"o"}")
s = S.new(1)
p s.respond_to?("zz#{1}".to_sym), s.respond_to?("q#{1}"), s.respond_to?(:hash)
