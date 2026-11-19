# spinel: gc-stress
class UnpackFormat
  def initialize(target)
    @target = target
  end

  def to_str
    @target.replace("Z" * 4096)
    "C"
  end
end

class UnpackPeer
  def unpack1(format)
    :peer
  end
end

def unpack_value(value, format)
  value.unpack1(format)
end

text = +"AB"
p unpack_value([text, UnpackPeer.new].first, [UnpackFormat.new(text), 1].first)
p text.bytesize
p unpack_value([UnpackPeer.new, text].first, "C")
