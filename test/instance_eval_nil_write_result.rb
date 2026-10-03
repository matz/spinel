# The assignment's nil result and the rebound ivar have separate representations.
class IntegerReceiver
  attr_reader :x
  def initialize = (@x = 1)
end
class StringCaller
  def initialize = (@x = 'caller')
  def run(receiver)
    p receiver.instance_eval { @x = nil }
    p receiver.x
    p @x
  end
  def exec(receiver)
    p receiver.instance_exec { @x = nil }
    p receiver.x
    p @x
  end
end
StringCaller.new.run(IntegerReceiver.new)
StringCaller.new.exec(IntegerReceiver.new)
class StringReceiver
  attr_reader :x
  def initialize = (@x = 'receiver')
end
class IntegerCaller
  def initialize = (@x = 2)
  def run(receiver)
    p receiver.instance_eval { @x = nil }
    p receiver.x
    p @x
  end
end
IntegerCaller.new.run(StringReceiver.new)
