# A boxed class value answers Class#superclass even when a class of the
# program, or Object, has an instance method named superclass. A module
# value has no Class#superclass, so it takes Object's method or raises.
class Node
  def superclass = "node"
end
class K; end
module M; end
[K, Node.new, Integer].each { |c| p c.superclass }
class Object
  def superclass = "pub"
end
[K, M, 3, Integer, Comparable].each { |c| p c.superclass }
