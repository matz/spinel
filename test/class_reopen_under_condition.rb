# A `class Class` inside a conditional is Class's reopening, as one at the
# top level is: its methods answer on every class (it stayed a plain class
# named Class, whose arm read a class value as an object pointer), and with
# a top-level copy of the same definitions each class keeps its arm.
unless $loaded
  class Class
    def kind_tag = [:class, name]
  end
end

$loaded = true
class Class
  def label = [:label, name]
end
unless $loaded
  class Class
    def label = [:label, name]
  end
end

class A; end
class B
  def kind_tag = :b
  def label = :b
end

xs = [A, B.new, 1]
xs.each do |x|
  p(x.respond_to?(:kind_tag) ? x.kind_tag : :none)
  p(x.respond_to?(:label) ? x.label : :none)
end
p A.kind_tag
p A.label
p String.label
