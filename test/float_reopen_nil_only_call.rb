# A Float or Integer reopening's to_a / to_h / =~ / & answers the call,
# not nil's method of the name (CRuby calls the reopening's method).
class Float
  def to_a = "RE:to_a"
  def to_h = {k: self}
  def =~(o) = "match #{o}"
  def &(o) = "and #{o}"
end
class Integer
  def to_h = {i: self}
end
x = 2.5.to_a
p x
p 1.5.to_h
f = 3.0
p(f =~ 1)
p(f & 2)
def g(v) = v.to_a
p g(4.5)
p [1.5, 2.5].map(&:to_a)
p 3.to_h
p((3.0.to_a + "!").size)
