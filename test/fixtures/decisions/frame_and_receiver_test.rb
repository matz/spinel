# The receiver test of a boxed dispatch reads `... && _t1.cls_id`: the `&&`
# is the logical operator, so the receiver's temp is a slot of the frame, not a
# registered root.
def consume(coll)
  coll.include?(:x)
end

class Box
  attr_accessor :v
end

b = Box.new
b.v = [:y, :x]
puts consume(b.v)
b.v = {x: 1}
puts consume(b.v)
