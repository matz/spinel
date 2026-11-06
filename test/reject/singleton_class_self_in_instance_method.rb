# `class << self` inside an instance method opens the singleton class of
# the one instance that runs it, whose defs a compiled class cannot hold:
# refused where the method's body is emitted (#11157).
class Box
  def initialize(v)
    @v = v
  end

  def stub
    class << self
      def with
        yield @v
      end
    end
  end
end
b = Box.new(3)
b.stub
b.with { |x| p x }
