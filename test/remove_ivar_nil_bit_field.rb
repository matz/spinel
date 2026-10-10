# remove_instance_variable answers the field it removes: an Integer or Float
# field with a nil bit answers its nil, as instance_variable_get does.
class P
  def initialize(x, f)
    @y = x
    @f = f
  end
  def set(v); @y = v; end
  def setf(v); @f = v; end
end
c = P.new(9, 1.5)
p c.remove_instance_variable(:@y)
c.set(nil)
p c.remove_instance_variable(:@y)
c.set(4)
x = c.remove_instance_variable("@y")
p x
c.setf(nil)
p c.remove_instance_variable(:@f)
c.setf(2.5)
p c.remove_instance_variable(:@f) + 1
