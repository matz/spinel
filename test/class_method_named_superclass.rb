# A class method of the program named superclass answers before Class#superclass.
class K
  def self.superclass = "own"
end
p K.superclass

module Ext
  def superclass = "ext"
end
class J
  extend Ext
end
p J.superclass

class L < K; end
p L.superclass

module A
  class N
    def self.superclass(x = 1) = [x, 2]
  end
end
p A::N.superclass
p A::N.superclass(5).size

class Plain; end
p Plain.superclass
