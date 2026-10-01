# A **kwrest reached through a dispatch on a receiver of several classes,
# with no keywords passed, is an empty Hash, not nil.
class Img
  def a(**o) = o
  def b(*r, **o) = [r, o]
  def c(x = 1, **o) = [x, o]
end
class Other
  def a = 0
  def b = 0
  def c = 0
end
y = [Img.new, Other.new][0]
p y.a, y.b, y.c
p y.a.is_a?(Hash), y.a.empty?
p y.b(1), y.b(k: 1), y.c(2, k: 3)
