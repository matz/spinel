# A user exception class's own method on a value that also holds another
# class: a boxed exception carries the builtin exception tag, not its class's
# index, so the dispatch's arm for the class never matched and the call raised
# NoMethodError (#5093). The key now maps a boxed exception to its class.

class BaseError < StandardError
  def message
    "located"
  end
end

class Detailed < BaseError
  attr_reader :code
  def initialize(code)
    super("detailed")
    @code = code
  end
  def describe = "#{self.class.name} #{code}"
end

class Thing
  def message = "thing"
  def describe = "a thing"
end

begin
  raise BaseError, "x"
rescue BaseError => e
  puts e.message
end
e = Thing.new
puts e.message

begin
  raise Detailed.new(42)
rescue BaseError => f
  puts f.message
  puts f.describe
end
f = Thing.new
puts f.describe
