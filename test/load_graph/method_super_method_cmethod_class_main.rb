# The super_method of a class method that takes its receiving class keeps the receiver the Method
# names: through call, to_proc and a local, on a subclass that is instantiated.
class P
  def self.make = new
  def self.tag = self
end
class C < P
  def self.make = super
  def self.tag = super
end
class D < C; end
p C.method(:make).super_method.call.class
p C.method(:make).super_method.to_proc.call.class
p D.method(:make).super_method.call.class
p D.method(:make).super_method.to_proc.call.class
m = D.method(:tag)
p m.super_method.call
p m.super_method.to_proc.call
p m.super_method.super_method
