# A class's own class method comes before the modules the class extends, and
# its super reaches the extended module's method, as CRuby's singleton
# ancestors run. The module's copy was skipped because the class already
# had the name, so the super raised NoMethodError.
module E
  def foo = [:e]
end
class K
  extend E
  def self.foo = [:own, *super]
end
p K.foo

# the own method written before the extend, with an argument
module E2
  def bar(x) = x * 2
end
class K2
  def self.bar(x) = super(x) + 1
  extend E2
end
p K2.bar(5)

# behind the own method, the later module first, each reaching the earlier
module A1
  def who = [:a1]
end
module B1
  def who = [:b1, *super]
end
class K3
  extend A1
  extend B1
  def self.who = [:own, *super]
end
p K3.who

# an own method without super still hides the module's
class K4
  extend E
  def self.foo = [:only]
end
p K4.foo

# a yielding module method behind the own one
module Y
  def each_pair = yield(1, 2)
end
class K5
  extend Y
  def self.each_pair = super { |a, b| a + b }
end
p K5.each_pair
