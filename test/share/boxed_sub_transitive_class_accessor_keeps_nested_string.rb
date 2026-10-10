# spinel: share
# The outer boxed call has only String and instance-method arms. The instance
# arm returns a Hash from a class helper that reads a published class ivar;
# Payload intentionally defines no class-side `sub` method.
class Payload
  class << self
    attr_accessor :payload

    def box
      { "payload" => @payload }
    end
  end
end

class OtherArm
  def sub(pattern, replacement)
    Payload.box
  end
end

source = String.new("https://")
Payload.payload = source

[source, OtherArm.new].each do |receiver|
  result = receiver.sub("://", "")
  if result.is_a?(Hash)
    nested = result.fetch("payload")
    puts "nested_is_source=#{nested.equal?(source)}"
    nested << "!"
  elsif result.is_a?(String)
    result << "!"
  end
end
