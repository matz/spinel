# A TrueClass or FalseClass method takes the types of the arguments its
# calls pass: before, the parameter kept the type of its default only, and
# the argument was converted to it (`true.tag(true)` gave 1).
class TrueClass
  def tag(a = 1) = a
  def me(a = self) = a
  def pair(a = 1, b = self) = [a, b]
  def kw(k: 1) = k
end
class FalseClass
  def tag(a = 2) = [a]
  def pair(a, b = self) = [a, b]
end

p true.tag
p true.tag(true)
p true.tag("s")
p true.tag(1.5)
p true.me
p true.me(3)
p true.me("x")
p true.pair
p true.pair(2, 3)
p true.kw
p true.kw(k: :x)
p false.tag
p false.tag("f")
p false.pair(1)
p false.pair(1, "b")

def go(v) = false.pair(v)
p go(1), go("s")

[true, false].each { |b| p b.tag(:sym) }
y = 3 > 2
p y.tag([1, 2])

# a name only TrueClass defines: false reaches Object's, which takes the
# arguments too
class TrueClass; def only_t(a = 1) = a; end
class Object; def only_t(a = 9) = [:obj, a]; end
p true.only_t("s"), false.only_t("f"), false.only_t, 3.only_t(2.5)
