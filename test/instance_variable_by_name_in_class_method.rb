# instance_variable_get, _set and _defined? on self in a class method: the
# class's own ivar, as @x written there is, each subclass with its own;
# in an instance method, the instance's.
class IcReg
  @own = 1
  def self.get = instance_variable_get(:@own)
  def self.set(v) = instance_variable_set(:@own, v)
  def self.has = instance_variable_defined?(:@own)
  def self.hasnt = instance_variable_defined?(:@nope)
  def self.bump
    instance_variable_set(:@own, instance_variable_get(:@own) + 1)
    instance_variable_get(:@own)
  end
  class << self
    def tags = instance_variable_get(:@tags)
    def tag(t)
      instance_variable_set(:@tags, (tags || []) + [t])
      self
    end
  end
  def initialize = @own = :instance
  def get = instance_variable_get(:@own)
end
class IcSub < IcReg
end
IcReg.set(5)
p IcReg.get, IcReg.has, IcReg.hasnt, IcReg.bump, IcReg.tag(:a).tag(:b).tags, IcReg.new.get
p IcSub.get, IcSub.has
IcSub.set(9)
p IcSub.get, IcReg.get
