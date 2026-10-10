# spinel: share
# A boxed String/class/instance receiver union must retain a nested String
# returned from a reachable class-value Hash arm.
class Params
  class << self
    attr_accessor :payload

    def sub(pattern, replacement)
      { "payload" => @payload }
    end
  end
end

class OtherArm
  def sub(pattern, replacement)
    nil
  end
end

source = String.new("https://")
Params.payload = source
seed = Params.sub("seed", "")
puts "seed_payload_is_source=#{seed.fetch("payload").equal?(source)}"

[source, Params, OtherArm.new].each do |receiver|
  result = receiver.sub("://", "")
  if result.is_a?(Hash)
    nested = result.fetch("payload")
    puts "nested_is_source=#{nested.equal?(source)}"
    nested << "!"
  elsif result.is_a?(String)
    result << "!"
  end
end
