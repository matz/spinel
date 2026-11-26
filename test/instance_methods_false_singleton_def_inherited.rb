# spinel: not-cruby -- CRuby lists the receiving class's own methods ([:a1], [:b1], []); Spinel raises NotImplementedError for a `class << self` def that a subclass can receive
# instance_methods(false) in a `class << self` def of a class with a
# subclass does not return another class's list.
class A
  class << self
    def own = instance_methods(false).sort
  end
  def a1 = 1
end

class B < A
  def b1 = 1
end

[A, B].each do |k|
  begin
    p k.own
  rescue NotImplementedError => e
    puts e.message
  end
end
