# send(:name) on a boxed value: a value whose class has no own method of
# the name runs the top-level def (Object's). Integer and Symbol take
# Kernel's hash, so they reach `def hash`. Only Hash has `default`, so the
# other values reach `def default`, and the Hash work for the Hash arm
# does not run for them.
def hash = 42
def default = :top
def update(x) = [:top, x]
def captures = :cap
p [3, :a].map { |v| v.send(:hash) }
p [3, "s"].map { |v| v.send(:hash) == 42 }
p [3, "s"].map { |v| v.send(:default) }
p [3, Hash.new(7)].map { |v| v.send(:default) }
p [3, "s", nil].map { |v| v.send(:update, 1) }
p [3, {a: 1}].map { |v| v.send(:update, {b: 2}) }
p [3, "s"].map { |v| v.send(:captures) }
x = [3, "a"].sample
p x.send(:default)
# A class of the program under a builtin class takes that builtin's
# method first, as the builtin class does.
def to_s = "TOP"
class MyH < Hash; end
class MyS < String; end
module Ns; class MyA < Array; end; end
p [MyH.new, 3].map { |v| v.send(:default) }
p [MyS.new("x"), 3].map { |v| v.send(:to_s) }
p [Ns::MyA.new, 3].map { |v| v.send(:hash) == 42 }
