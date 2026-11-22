# An included hook that sets and reads the including class's instance
# variable by name on its parameter: as a statement, under an if, as a
# value read, and with self in a class_eval block.
module IvsHooked
  def self.included(base)
    base.instance_variable_set(:@flag, :hooked)
    p base.instance_variable_get(:@flag)
    base.instance_variable_set(:@list, [base.name])
    x = base.instance_variable_get(:@list)
    p x
  end
end
class IvsA
  include IvsHooked
  def self.flag = @flag
  def self.list = @list
end
class IvsB
  @flag = :own
  include IvsHooked
  def self.flag = @flag
  def self.list = @list
end
p IvsA.flag, IvsB.flag, IvsA.list, IvsB.list

module IvsDeep
  def self.included(base)
    base.instance_variable_set(:@last, 2)
    if base.name.start_with?("Ivs")
      base.instance_variable_set(:@named, true)
    end
    v = base.instance_variable_get(:@last)
    p v
    base.class_eval do
      instance_variable_set(:@inner, 7)
    end
  end
end
class IvsC
  include IvsDeep
  def self.last = @last
  def self.named = @named
  def self.inner = @inner
end
p IvsC.last, IvsC.named, IvsC.inner
