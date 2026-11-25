class Parent
  def hash = 7
end

class Child < Parent
  attr_reader :hash
  def initialize = @hash = {}
end

entries = {}
child = entries[:child] ||= Child.new
child.hash[:key] = 42
p child.hash[:key]
