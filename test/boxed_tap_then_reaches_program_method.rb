# A value held boxed (one of several classes, or nil) whose class defines
# its own tap or then calls that method; a value of any other class runs
# the builtin, which hands the block the value and answers it (tap) or the
# block's value (then). In value position: assigned, printed, an argument,
# through `&.`, in a chain. A builtin class's reopening counts as its own.

class Foo
  def tap = :foo
  def then = :foo_then
end
class Integer
  def then(&b) = [:int_then, b ? b.call(self) : nil]
end
class Bar
  def initialize(n) = @n = n
  def then(&b) = [:bar, b.call(@n)]
end

vals = [1, "s", nil, Foo.new, 2.5, :sym, [1, 2], {a: 1}, Bar.new(7)]
vals.each do |v|
  x = v.tap { |o| 0 }
  y = v.then { |o| o.class }
  p [x.class, y]
end

def via(v) = v.then { |o| o.to_s + "!" }
p via(3)
p via(Bar.new(2))

s = [+"ab", Foo.new].first
s.tap { |t| t << "c" }
p s
r = [3, Foo.new].first.tap { |q| p [:in, q] }
p r
z = [nil, 4].last&.then { |q| q * 3 }
p z
w = [Foo.new, nil].first&.tap { 1 }
p w
p [1, Foo.new].map { |v| v.then { |o| o.is_a?(Integer) ? o + 1 : o } }.map(&:class)

y = [7, nil].first
p y&.then { |v| v + 1 }
n = [nil, 7].first
p n&.then { |v| v + 1 }

# the block's own calls are not the arm's: a program method of another
# value's class, and its respond_to?, answer inside the builtin's block
class Sized
  def respond_to?(m, priv = false) = m == :zz ? true : super
  def size = 99
end
ws = [Sized.new, [1, 2], "abc"]
[1, Foo.new, "s"].each do |v|
  x = v.tap { |o| p [ws.first.size, ws.map { |w| w.respond_to?(:zz) }] }
  y = v.then { |o| ws.map { |w| w.size } }
  p [x.class, y]
end
