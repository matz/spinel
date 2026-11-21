# An attr_accessor and a def of one name, made by one block a class body
# runs: the list of the class's own instance methods reads the versions'
# cells, which the plan keeps for the list, and lists the name once one of
# them has run; a block that never runs makes neither.
class K
  [1, 2].each { |n| n == 1 ? attr_accessor(:m) : (def m = 2) }
end
class J
  [].each { |n| n == 1 ? attr_accessor(:m) : (def m = 2) }
end
class L
  [1, 2].each { |n| n == 1 ? define_method(:m) { 1 } : attr_reader(:m) }
end
p K.instance_methods(false).include?(:m)
p K.public_instance_methods(false).include?(:m)
p J.instance_methods(false).include?(:m)
p L.instance_methods(false).include?(:m)
p K.new.m
