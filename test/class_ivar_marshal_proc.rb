# A Proc held in a class-level ivar is the class object's: dumping an
# instance does not reach it.
class A
  @hook = ->(x) { x * 2 }
  def self.hook = @hook
  def initialize = @v = 3
  attr_reader :v
end
b = Marshal.load(Marshal.dump(A.new))
p b.v
p b.instance_variables
p A.hook.call(4)
