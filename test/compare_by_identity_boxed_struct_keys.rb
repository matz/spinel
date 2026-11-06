# Hash#compare_by_identity keyed by objects of two Struct classes read out
# of an Array in a block (#11078): the key is boxed, and the class facts'
# kinds show it holds nothing but objects of the program's classes, so the
# hash is the identity hash CRuby makes
A = Struct.new(:x)
B = Struct.new(:y)
a1 = A.new(1)
keys = [a1, B.new(2), A.new(1), a1]
h = {}.compare_by_identity
keys.each { |k| h[k] ||= []; h[k] << k.to_a.first }
p h.size
p h[a1]
p h.key?(A.new(1))
p h.values.map(&:size)
