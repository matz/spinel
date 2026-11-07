# A local that holds a class method's self, then its superclass in a loop,
# answers the reflection methods for whichever class it holds
class Base
  def self.base? = (self == Base)
  def self.own_lists
    k = self
    lists = []
    until k.base?
      lists << k.public_instance_methods(false).sort
      k = k.superclass
    end
    lists
  end
  def self.internal_methods
    k = self
    methods = []
    until k.base?
      methods += k.public_instance_methods(false)
      k = k.superclass
    end
    k.public_instance_methods(true) - methods
  end
  def base_m = 0
end
class Mid < Base
  def mid_m = 1
  def mid_n = 2
  private def mid_p = 3
end
class Leaf < Mid
  def index = 4
  def show = 5
end
p Leaf.own_lists
p Mid.own_lists
p Base.own_lists
p Leaf.internal_methods.include?(:base_m)
p Leaf.internal_methods.include?(:index)
p Leaf.internal_methods.include?(:mid_m)
p Leaf.instance_methods(true).include?(:mid_m)
