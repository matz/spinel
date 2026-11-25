module Codec
  Pairs = lambda { |value|
    return value unless value.respond_to?(:each_slice)
    value.each_slice(2).map { |a, b| [a, b * 2] }
  }
  Arrow = ->(value) {
    return :none if value.nil?
    value
  }
end
