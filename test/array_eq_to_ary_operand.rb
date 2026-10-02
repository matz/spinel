class Step
  attr_reader :name, :note

  def initialize(name, note)
    @name = name
    @note = note
  end

  def ==(other) = other.is_a?(Step) && other.name == name
end

class Trail
  def initialize(items)
    @items = items
  end

  def ==(other) = @items == other
  def to_ary = @items
end

class Plain
  def ==(other) = true
end

class Lazy
  def to_ary
    puts "to_ary called"
    nil
  end

  def ==(other) = :lazy
end

def same?(a, b) = a == b

p same?([Step.new(:a, 1)], Trail.new([Step.new(:a, 2)]))
p same?([[Step.new(:a, 1)]], Trail.new([Trail.new([Step.new(:a, 2)])]))
p same?([Step.new(:b, 1)], Trail.new([Step.new(:a, 1)]))
p same?([1], Plain.new)
p same?(Plain.new, [1])
p same?([1], Lazy.new)
