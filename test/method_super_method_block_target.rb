# Method#super_method of a method that takes a block, whose target is behind
# an include, behind a prepend or in the parent class: the block is passed on.
module M
  def go(&b) = [:m, b&.call]
end
class A
  include M
  def go(&b) = [:a, super]
end
p A.new.go { 3 }
p A.new.method(:go).super_method.call { 4 }

module P
  def run(&b) = [:p, super]
end
class B
  prepend P
  def run(&b) = [:b, b&.call]
end
p B.new.run { 3 }
p B.new.method(:run).super_method.call { 4 }

class Q
  def walk(&b) = [:q, b&.call]
end
class C < Q
  def walk(&b) = [:c, super]
end
p C.new.method(:walk).super_method.call { 4 }
