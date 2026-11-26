# A Hash subclass whose initialize has an optional parameter, nil by
# default, compiles and runs: an Integer, a Float or a String default kept
# in an instance variable beside super()
class IntOpt < Hash
  attr_reader :d
  def initialize(d = nil)
    @d = d
    super()
  end
end

class FloatOpt < Hash
  attr_reader :d
  def initialize(d = nil, &blk)
    @d = d
    super(&blk)
  end
end

class StrOpt < Hash
  attr_reader :d
  def initialize(d = nil)
    @d = d
    super()
  end
end

p IntOpt.new.d
p IntOpt.new(3).d
p FloatOpt.new.d
p FloatOpt.new(1.5) { |h, k| k.to_s }[:z]
p StrOpt.new("s").d
x = IntOpt.new(4)
x[:a] = 1
p x, x.class
