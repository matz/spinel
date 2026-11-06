# A module value has no Class#superclass: beside a program class's own
# instance method of that name, a boxed module still raises NoMethodError.
class Node
  def superclass = "node"
end
module M; end
[M, Comparable, Node.new, Integer].each { |c| p((c.superclass rescue $!.message)) }
