# An Object.new instance keeps the instance variables instance_variable_set
# gives it, as in CRuby: instance_variable_get reads them back,
# instance_variable_defined? and instance_variables see them, a copy takes
# its own, and a frozen one refuses a new one. The write was dropped and
# every read answered nil, and instance_variables raised NoMethodError.

o = Object.new
p o.instance_variable_get(:@a), o.instance_variable_defined?(:@a), o.instance_variables
p o.instance_variable_set(:@a, 5)
o.instance_variable_set("@b", "two")
p o.instance_variable_get(:@a), o.instance_variable_get("@b")
p o.instance_variable_defined?(:@a), o.instance_variable_defined?(:@c), o.instance_variables
o.instance_variable_set(:@a, [1, 2])
p o.instance_variable_get(:@a)

d = o.dup
d.instance_variable_set(:@a, :changed)
p o.instance_variable_get(:@a), d.instance_variable_get(:@a), d.instance_variable_get(:@b)

f = Object.new
f.instance_variable_set(:@x, 1)
f.freeze
p(begin; f.instance_variable_set(:@x, 2); rescue FrozenError => e; e.class; end)
p f.instance_variable_get(:@x)

class K; def initialize = (@k = 3); end
xs = [Object.new, K.new]
xs[0].instance_variable_set(:@k, "obj")
p xs.map { |x| x.instance_variable_get(:@k) }
p xs.map { |x| x.instance_variables }
p xs.map { |x| x.instance_variable_defined?(:@k) }
