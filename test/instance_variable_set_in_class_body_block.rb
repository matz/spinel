# instance_variable_set and instance_variable_get on self inside a block of
# a class body: the block keeps the body's self, so they are the class's
# ivar. A block with a self of its own (define_method's, instance_eval's)
# is left alone. In an included hook, a block that sets the includer's.
class BsA
  [1, 2].each { |t| instance_variable_set(:@last, t) }
  %w[x y].each do |n|
    instance_variable_set(:@names, (instance_variable_get(:@names) || []) + [n])
  end
  def self.last = @last
  def self.names = @names
end
p BsA.last, BsA.names

class BsHolder
  attr_reader :v
end
class BsB
  define_method(:setv) { instance_variable_set(:@v, 5); self }
  def v = @v
  h = BsHolder.new
  h.instance_eval { instance_variable_set(:@v, 7) }
  HV = h.v
  S = Struct.new(:a) do
    def tag = instance_variable_get(:@t)
  end
  @own = 1
  [3].each { |t| instance_variable_set(:@own, instance_variable_get(:@own) + t) }
  def self.own = @own
end
p BsB.new.setv.v, BsB::HV, BsB.own

module BsTag
  def self.included(base)
    [:a, :b].each { |t| base.instance_variable_set(:@last_tag, t) }
  end
end
class BsC
  include BsTag
  def self.last_tag = @last_tag
end
p BsC.last_tag
