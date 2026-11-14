# A constant the program assigns under a builtin class's name is not a
# reopening of that class: its class entry is the program's own class, and
# is_a? or `when` on it does not take the builtin's values.
$VERBOSE = nil
P = Data.define(:x)
pv = P.new(x: 1)
Data = Struct.new(:a)
w = [pv, Data.new(1), 5]
p w.map { |x| [x.is_a?(Data), x.instance_of?(Data), (case x when Data then 1 else 0 end)] }

BasicObject = Class.new
b = [5, "s", BasicObject.new]
p b.map { |x| [x.is_a?(BasicObject), (case x when BasicObject then 1 else 0 end)] }
