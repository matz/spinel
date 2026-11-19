# frozen_string_literal: true

class HashTransformConversion
  def initialize(text)
    @text = text
  end

  def to_str
    GC.start
    @text
  end

  def to_s
    "display"
  end
end

raw = +"raw"
converted = +"converted"
source = { :raw => raw, 7 => HashTransformConversion.new(converted) }
a = source.transform_values { |value| String(value) }
b = source.transform_values { |value| value.to_s }
c = source.transform_values do |value|
  next raw if value.equal?(raw)
  converted
end
p [a[:raw].equal?(raw), a[7].equal?(converted), b[:raw].equal?(raw), b[7]]
p [c[:raw].equal?(raw), c[7].equal?(converted)]
