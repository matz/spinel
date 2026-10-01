class Matcher
  attr_reader :values
  def initialize(values = []) = @values = values
end
class AllMatcher < Matcher
  def self.instance = INSTANCE
  INSTANCE = new
  private_class_method :new
end
class Loop < Matcher
  private_class_method :new
  ONE = new([1])
  def self.one = ONE
end
p AllMatcher.instance.values
p Loop.one.values
p AllMatcher.instance.equal?(AllMatcher.instance)
begin
  AllMatcher.new
rescue NoMethodError => e
  p e.message
end
