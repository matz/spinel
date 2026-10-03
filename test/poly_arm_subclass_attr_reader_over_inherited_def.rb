# A subclass `attr_reader` wins over an inherited `def` on a poly receiver.
class Base
  def seek_to = 0.0
end

class Window < Base
  attr_reader :seek_to

  def initialize = @seek_to = 38.0
end

class Other < Base
end

[Window.new, Other.new].each { |o| p o.seek_to }
