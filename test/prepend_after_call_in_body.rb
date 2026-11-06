# A prepend in a class's first body, after a call in that body: the call
# ahead of it reaches the class's own method, the ones after it the
# module's, on the instance side and on the singleton side (#10790).
module Wrap; def hi = "W>" + super; end
class K
  def hi = "K"
  k = K.new
  p k.hi
  prepend Wrap
  p k.hi, K.new.hi
end
p K.new.hi, K.ancestors.first

module Loud; def hi = "L>" + super; end
class S
  def self.hi = "S"
  p hi
  singleton_class.prepend(Loud)
  p hi
end
p S.hi

# no call ahead: the prepend holds from the body's start, as before
class Plain
  prepend Wrap
  def hi = "P"
end
p Plain.new.hi
