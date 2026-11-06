# `def self.m` inside an instance method defines a method on the one
# instance that runs it, which a compiled class cannot hold: refused where
# the method's body is emitted, instead of compiling to a class method that
# no call on the instance reaches (#11157).
class Box
  def initialize(v)
    @v = v
  end

  def stub
    def self.with
      yield @v
    end
  end
end
b = Box.new(3)
b.stub
b.with { |x| p x }
p Box.new(4).respond_to?(:with)
