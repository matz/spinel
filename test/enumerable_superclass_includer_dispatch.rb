# A subclass of a class that includes Enumerable answers Enumerable's
# block-taking methods when the call goes through a by-name dispatch (the
# receiver is poly and another class defines the name itself).
class Node
  include Enumerable
  attr_reader :left
  def initialize(left) = @left = left
  def each(&block)
    block.call(self)
    block.call(left) if left
  end
  def group? = false
  def weight = 1
end

class Binary < Node
  attr_reader :right
  def initialize(left, right)
    super(left)
    @right = right
  end
end

class Cat < Binary
  def group? = true
  def weight = 10
end

class Or < Binary
  def find_all = [:or_own]
end

class Matcher
  def find_all = [:own]
  def map = [:own_map]
  def count = -1
end

specs = [Cat.new(Node.new(nil), nil), Node.new(nil), Matcher.new, Or.new(nil, nil)]
specs.each do |spec|
  p spec.find_all(&:group?).size
  p spec.map { |n| n.class.name }
  p spec.count { |n| n.weight > 5 }
end
