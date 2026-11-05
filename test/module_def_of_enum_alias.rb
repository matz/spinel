# A module's own instance method of an Enumerable alias's name (find_all)
# is its includers' method; every other receiver's find_all is still
# Enumerable's select.
module ViewPaths
  def find_all(name, prefix = nil) = [:view_paths, name, prefix]
end
class LookupContext
  include ViewPaths
end
class Node
  include Enumerable
  def initialize(*kids) = @kids = kids
  def symbol? = false
  def each(&block)
    block.call(self)
    @kids.each { |k| k.each(&block) }
  end
end
class Sym < Node
  def symbol? = true
end
tree = Node.new(Sym.new, Node.new(Sym.new))
p tree.find_all(&:symbol?).size, tree.find_all { |n| !n.symbol? }.size
p [1, 2, 3, 4].find_all(&:even?), { a: 1, b: 2 }.find_all { |k, v| v > 1 }
p LookupContext.new.find_all("index", "posts")
# a module that answers its own methods (extend self) keeps the name for its
# own calls
module Finder
  extend self
  def detect(x) = [:finder, x]
end
p Finder.detect(1)
p [5, 6].detect(&:even?)
