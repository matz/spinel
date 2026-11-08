# A receiverless call with a block inside a class method reaches the class
# method of that name, ahead of a same-named instance method or top-level
# def, so the block a recursive class method passes itself captures what it
# reads. The shape is tsort's `return to_enum(__method__, ...) unless
# block_given?` beside an instance method of the same name.
class Graph
  def self.walk(n)
    return to_enum(__method__, n) unless block_given?
    yield n
    walk(n - 1) { |v| yield v } if n > 0
  end

  def walk = Graph.walk(2) { |v| p v }
end

Graph.walk(2) { |v| p v }
p Graph.walk(3).to_a
Graph.new.walk

class Pairs
  def self.doubles(n)
    return Enumerator.new { |y| doubles(n) { |v| y << v } } unless block_given?
    yield n * 2
  end

  def doubles = 0
end

p Pairs.doubles(4).to_a

# A top-level def of the name sits below the class method.
def steps = 0

class Steps
  def self.steps(n)
    return to_enum(__method__, n) unless block_given?
    n.times { |i| yield i }
  end
end

p Steps.steps(3).to_a

# An instance method still reaches the instance method of the name.
class Tree
  def leaves(n)
    return to_enum(__method__, n) unless block_given?
    yield n
  end

  def self.leaves = 0
end

p Tree.new.leaves(5).to_a
