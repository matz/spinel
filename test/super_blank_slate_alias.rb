# A super in a class under BasicObject, its superclass written through a
# constant alias (top-level or in a module): Object is not on its chain,
# so a method the program defines on Object is not its superclass method,
# and the super raises NoMethodError. The program's own BasicObject's
# method is.

class Object
  def frob = :obj
end

Base = BasicObject
class X1 < Base
  def frob = super
end

module P
  Kept = ::BasicObject
  class X2 < Kept
    def frob = super
  end
end

class Y < Object
  def frob = super
end

def try
  ::Kernel.p yield
rescue ::NoMethodError
  ::Kernel.p :nme
end

try { X1.new.frob }
try { P::X2.new.frob }
try { Y.new.frob }
