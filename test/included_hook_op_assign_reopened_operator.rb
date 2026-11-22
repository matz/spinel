# An operator-assign in an included hook, with Integer#+ reopened: the
# local, the element and the attribute of the includer each call the
# reopened operator, as the statement outside a hook does.
class Integer
  def +(o) = self - o
end
module OpHook
  def self.included(base)
    n = 10
    n += 1
    p n
    a = [5]
    a[0] += 2
    p a
    base.class_eval do
      self.total += 3
    end
  end
end
class OpA
  class << self; attr_accessor :total; end
  self.total = 20
  include OpHook
end
p OpA.total
x = 4
x += 1
p x
