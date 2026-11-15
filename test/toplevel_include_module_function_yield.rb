# A module of module functions with a yielding method, included at the top
# level: Object's too. A call of it in a class method runs Object's copy
# (the class object is an Object), not the module's own.
module T
  module_function
  def twice = yield + yield
  def plain = 3
end
include T
p twice { 2 }
p T.twice { 5 }
p plain, T.plain
class K
  def self.cm(x) = twice { x }
  def im(x) = twice { x + 1 }
end
p K.cm(4), K.new.im(4)
def top(x) = twice { x * 2 }
p top(3)
