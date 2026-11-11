# A run-time dispatch over two modules whose singleton method takes a String
# in one and, by its RBS signature, an Array in the other: the arm whose
# parameter the argument cannot fit raises the signature's TypeError where it
# runs, and is no reason to refuse the call. Through a parameter and an ivar.
module Inflector
  #: (String) -> String
  def self.classify(name)
    name.capitalize
  end
end

module Agent
  #: (Array[String]) -> String
  def self.classify(tokens)
    tokens.empty? ? "base" : tokens.last
  end
end

puts Agent.classify(["a", "b"])

def through(m)
  m.classify("gear")
end
puts through(Inflector)

class Holder
  def initialize(m) = @m = m
  def run = @m.classify("cog")
end
puts Holder.new(Inflector).run
