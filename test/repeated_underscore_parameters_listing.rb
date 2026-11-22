# Proc#parameters and Method#parameters name a repeated underscore parameter
# as the program wrote it, in a proc, a lambda and a method.
pr = proc { |_a, _a| _a }
p pr.parameters
p pr.call(1, 2)
l = lambda { |_a, _a| _a }
p l.parameters
p l.call(3, 4)
p proc { |_, _| }.parameters
p lambda { |_, _| }.parameters
p proc { |_a, _a = 5, *_a| }.parameters
p lambda { |_x, _, _x, _| }.parameters

def m(_a, _a) = _a
def n(_, _, x = 1, _) = x
p method(:m).parameters
p method(:n).parameters
p m(5, 6)
p n(1, 2, 3)
