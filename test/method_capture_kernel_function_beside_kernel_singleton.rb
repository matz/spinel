# A Method of a Kernel function is Kernel's own method, whatever the program defines on Kernel's singleton
# (`def Integer` beside it, or after the capture) or at the top level.
class << Kernel
  def Integer(x) = "singleton"
  def Float(x) = "singleton"
end
m = method(:Integer)
f = method(:Float)
def Integer(x) = "top"
p m.call("5")
p f.call("2.5")
