# A class test of an Integer or Float slot that holds nil answers for nil:
# is_a?(Integer), kind_of?, instance_of? and `Integer === x` are false and
# NilClass is true. The test was folded from the slot's static type, so
# where the nullable-value analysis did not see the nil arrive -- a splat of
# a boxed Array, a proc called through another name, a method parameter
# only nil is passed to -- a nil answered true to is_a?(Integer).
s = [1]
_, y = *s
x = 1
p x
x = y
s = 5
p x.is_a?(Integer), x.kind_of?(Numeric), x.instance_of?(Integer), x.is_a?(NilClass)

pr = proc { |a, b| w = b; p [Integer === w, NilClass === w, w.is_a?(Object)] }
m = pr
m.call(1)
pr.call(1, 2)

def f(v) = v
z = 1.5
p z
z = f(nil) if ARGV.empty?
p [z.is_a?(Float), Float === z, z.is_a?(NilClass), z.is_a?(Comparable)]

n = 7
p [n.is_a?(Integer), Integer === n, 3.is_a?(Integer)]
