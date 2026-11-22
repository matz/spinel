# An include in a class_eval block of the body's own class is the class's:
# the module's methods answer, and a def after it in the block is on top.
module M; def hi = "hi #{name}"; end
class A
  def name = "a"
  class_eval do
    include M
    include Comparable
    def <=>(o) = name <=> o.name
  end
end
p A.new.hi, A.include?(M), A.new == A.new, A.ancestors.first(3)
