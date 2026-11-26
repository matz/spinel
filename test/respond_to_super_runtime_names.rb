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

# the names a builtin parent answers are its own methods, read at run time:
# Exception's, Struct's and Data's, and their members
names = %w[message full_message backtrace cause exception detailed_message set_backtrace inspect to_s zz]
p names.map { |n| E.new.respond_to?(n) }
p %w[x x= to_a members each zz].map { |n| s.respond_to?(n) }
D = Data.define(:y) do
  def respond_to?(*a) = super
end
d = D.new(y: 1)
p %w[y with to_h members zz].map { |n| d.respond_to?(n) }, d.respond_to?(:y)
class F < E; end
p [F.new.respond_to?("ca#{"use"}"), F.new.respond_to?("backtrace", true), F.new.respond_to?(:zz)]
q = [nil, "message"].last
p E.new.respond_to?(q), [E.new.respond_to?(q), s.respond_to?(q)]

# the builtin's own names come from CRuby's rows alone: a define_method that
# never runs adds none, and a visibility word the class says still counts
class G < StandardError
  def respond_to?(m, a = false) = super
  def self.install!
    define_method(:glow) { 1 } if false
  end
  private :full_message
end
G.install!
p [G.new.respond_to?(:glow), G.new.respond_to?("glow"), G.new.respond_to?(:message)]
p [G.new.respond_to?("full_#{"message"}"), G.new.respond_to?(:full_message, true)]
class H < StandardError
  def self.install!
    define_method(:glow) { 1 } if false
  end
end
H.install!
p [H.new.respond_to?(:glow), H.new.respond_to?(:message), [nil, H.new].last.respond_to?(:glow)]
