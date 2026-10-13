# Two refusals of the seal: a route (a call answering its String argument
# hands it to the boxed to_s as bytes) and a flow (an Object method's ivar
# read stored as an element). Without SP_COLLECT_ERRORS the run stops at
# the first; with it, it reports both (collect-errors-test).
class Own
  def initialize(n) = (@n = n)
  def to_s = @n
end
def tidy(o)
  s = o.to_s
  s << "!"
  [s, o.to_s]
end
b = +"b"
p tidy(Own.new(+"a")), tidy(b.itself), b
class Object
  def label = @label
end
class Labelled
  def initialize(s) = (@label = s)
end
class OtherLabel
  def label = +"other"
end
s = +"text"
objects = [Labelled.new(s), OtherLabel.new, 5]
labels = [objects[0].label, objects[1].label]
labels[0] << "!"
p labels, s, objects[2].label
