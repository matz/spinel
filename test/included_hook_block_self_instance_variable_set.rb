# instance_variable_set and instance_variable_get with no receiver in the
# class_eval block of an included hook: self is the including class there,
# as a statement, as a value and under an if.
module IvDeepSelf
  def self.included(base)
    base.class_eval do
      instance_variable_set(:@inner, 7)
      @direct = instance_variable_get(:@inner) + 1
      if instance_variable_get(:@inner) > 5
        instance_variable_set(:@big, true)
      end
    end
  end
end
class IvDsA
  include IvDeepSelf
  def self.inner = @inner
  def self.direct = @direct
  def self.big = @big
end
class IvDsB
  @inner = 1
  include IvDeepSelf
  def self.inner = @inner
end
p IvDsA.inner, IvDsA.direct, IvDsA.big, IvDsB.inner
