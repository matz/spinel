# Indirect writes keep nil in an Integer ivar, including the writer return.
# Separate classes keep a working route from changing another route's slot type.
def nil_value
  puts "nil argument"
  nil
end

class PublicNil
  attr_accessor :x, :y
  def initialize
    @x = 7
    @y = 8
  end
end
puts "PublicNil"
a1 = PublicNil.new
n = [:x=, :y=].first
a1.public_send(n, nil)
p a1.inspect.sub(/0x[0-9a-f]+/, "ADDR")
p [a1.x, a1.y, a1.x.nil?, a1.y.nil?]
a1.x = 0
p a1.x
a1.x = 9
p a1.x

class PublicOtherNil
  attr_accessor :x, :y
  def initialize
    @x = 7
    @y = 8
  end
end
puts "PublicOtherNil"
a2 = PublicOtherNil.new
n = [:x=, :y=].last
p a2.public_send(n, nil)
p a2.inspect.sub(/0x[0-9a-f]+/, "ADDR")
p [a2.x, a2.y, a2.x.nil?, a2.y.nil?]
a2.x = 0
p a2.x
a2.x = 9
p a2.x

class SendComputedNil
  attr_accessor :x, :y
  def initialize
    @x = 7
    @y = 8
  end
end
puts "SendComputedNil"
a3 = SendComputedNil.new
n = [:x=, :y=].first
p a3.send(n, nil)
p a3.inspect.sub(/0x[0-9a-f]+/, "ADDR")
p [a3.x, a3.y, a3.x.nil?, a3.y.nil?]
a3.x = 0
p a3.x
a3.x = 9
p a3.x

class SendLiteralNil
  attr_accessor :x, :y
  def initialize
    @x = 7
    @y = 8
  end
end
puts "SendLiteralNil"
a4 = SendLiteralNil.new
p a4.send(:x=, nil)
p a4.inspect.sub(/0x[0-9a-f]+/, "ADDR")
p [a4.x, a4.y, a4.x.nil?, a4.y.nil?]
a4.x = 0
p a4.x
a4.x = 9
p a4.x

class SetNil
  attr_accessor :x, :y
  def initialize
    @x = 7
    @y = 8
  end
end
puts "SetNil"
a5 = SetNil.new
p a5.instance_variable_set(:@x, nil)
p a5.inspect.sub(/0x[0-9a-f]+/, "ADDR")
p [a5.x, a5.y, a5.x.nil?, a5.y.nil?]
a5.x = 0
p a5.x
a5.x = 9
p a5.x

class MethodNil
  attr_accessor :x, :y
  def initialize
    @x = 7
    @y = 8
  end
end
puts "MethodNil"
a6 = MethodNil.new
p a6.method(:x=).call(nil)
p a6.inspect.sub(/0x[0-9a-f]+/, "ADDR")
p [a6.x, a6.y, a6.x.nil?, a6.y.nil?]
a6.x = 0
p a6.x
a6.x = 9
p a6.x

class EvalNil
  attr_accessor :x, :y
  def initialize
    @x = 7
    @y = 8
  end
end
puts "EvalNil"
a7 = EvalNil.new
p a7.instance_eval { @x = nil }
p a7.inspect.sub(/0x[0-9a-f]+/, "ADDR")
p [a7.x, a7.y, a7.x.nil?, a7.y.nil?]
a7.x = 0
p a7.x
a7.x = 9
p a7.x

class EvalStatementNil
  attr_accessor :x, :y
  def initialize
    @x = 7
    @y = 8
  end
end
puts "EvalStatementNil"
a8 = EvalStatementNil.new
a8.instance_eval { @x = nil; 1 }
p a8.inspect.sub(/0x[0-9a-f]+/, "ADDR")
p [a8.x, a8.y, a8.x.nil?, a8.y.nil?]
a8.x = 0
p a8.x
a8.x = 9
p a8.x

class ExecNil
  attr_accessor :x, :y
  def initialize
    @x = 7
    @y = 8
  end
end
puts "ExecNil"
a9 = ExecNil.new
p a9.instance_exec { @x = nil }
p a9.inspect.sub(/0x[0-9a-f]+/, "ADDR")
p [a9.x, a9.y, a9.x.nil?, a9.y.nil?]
a9.x = 0
p a9.x
a9.x = 9
p a9.x

class DirectWriterNil
  attr_accessor :x, :y
  def initialize
    @x = 7
    @y = 8
  end
end
puts "DirectWriterNil"
a10 = DirectWriterNil.new
p a10.x=(nil)
p a10.inspect.sub(/0x[0-9a-f]+/, "ADDR")
p [a10.x, a10.y, a10.x.nil?, a10.y.nil?]
a10.x = 0
p a10.x
a10.x = 9
p a10.x

class EffectNil
  attr_accessor :x, :y
  def initialize
    @x = 7
    @y = 8
  end
end
puts "EffectNil"
a11 = EffectNil.new
n = [:x=, :y=].first
p a11.public_send(n, nil_value)
p a11.inspect.sub(/0x[0-9a-f]+/, "ADDR")
p [a11.x, a11.y, a11.x.nil?, a11.y.nil?]
a11.x = 0
p a11.x
a11.x = 9
p a11.x

# The instance_eval receiver's slot type wins over the enclosing method's.
class NestedEvalReceiver
  attr_accessor :x
  def initialize
    @x = 7
  end
end
class NestedEvalWriter
  def initialize
    @x = "outer"
  end
  def write(receiver)
    receiver.instance_eval { @x = nil; 1 }
    p @x
  end
  def write_value(receiver)
    p receiver.instance_eval { @x = nil; :done }
    p @x
  end
end
puts "NestedEvalWriter"
receiver = NestedEvalReceiver.new
writer = NestedEvalWriter.new
writer.write(receiver)
p receiver.x
p receiver.x.nil?
receiver.x = 9
writer.write_value(receiver)
p receiver.x
p receiver.x.nil?
