# The ivar reflection on a builtin value -- a String, a number, an Array,
# a Hash, nil, a Symbol -- answers as CRuby does for a value nothing was
# set on: instance_variable_get is nil, instance_variables is empty and
# instance_variable_defined? false, and a set on a frozen kind (an Integer,
# nil) raises FrozenError. Each raised NoMethodError.

s = +"s"
p s.instance_variable_get(:@a), s.instance_variables, s.instance_variable_defined?(:@a)
p 5.instance_variable_get(:@a), 5.instance_variables, 5.instance_variable_defined?(:@a)
p [1].instance_variables, {a: 1}.instance_variable_get(:@x), nil.instance_variables, :sym.instance_variables
p 1.5.instance_variables, (1..2).instance_variables, true.instance_variable_get(:@q)
x = [s, 1][1]
p x.instance_variables, x.instance_variable_get(:@a), x.instance_variable_defined?(:@a)
p((5.instance_variable_set(:@a, 1) rescue $!.class))
p((nil.instance_variable_set(:@a, 1) rescue $!.message))
p((5.instance_variable_get(:a) rescue $!.class))
