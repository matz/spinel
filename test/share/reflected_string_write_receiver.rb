# frozen_string_literal: true

class ReflectionCell
  attr_reader :value

  def initialize(value)
    @value = value
  end
end

def reflected_write(receiver, other, value)
  original = receiver
  result = receiver.instance_variable_set(:@value, (receiver = other; GC.start; value))
  p [result.equal?(value), original.value.equal?(value), receiver.value]
end

def reflected_write_dropped(receiver, other, value)
  original = receiver
  receiver.instance_variable_set(:@value, (receiver = other; GC.start; value))
  p [original.value.equal?(value), other.value]
end

first = ReflectionCell.new(+"first")
other = ReflectionCell.new(+"other")
shared = +"value"
reflected_write(first, other, shared)
reflected_write_dropped(ReflectionCell.new(+"dropped"), other, shared)
shared << "!"
p first.value

target = ReflectionCell.new(+"held")
events = []
begin
  target.instance_variable_set(:@value, (target.freeze; events << :ran; GC.start; +"new"))
rescue FrozenError
  events << :frozen
end
p [events, target.value]
