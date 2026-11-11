# A local that holds a nested module (`m = Shop::Inflector`) calls that
# module's singleton method, not another module's of the same name, also
# when a top-level module of the same leaf exists
module Shop
  module Inflector
    def self.classify(name)
      name.capitalize
    end
  end
end

module Agent
  def self.classify(tokens)
    tokens.empty? ? "base" : tokens.last
  end
end

module Inflector
  def self.classify(name) = "top #{name}"
end

puts Agent.classify(["a", "b"])

m = Shop::Inflector
puts m.classify("gear")

t = Inflector
puts t.classify("gear")

k = Shop::Inflector
k = Shop::Inflector
puts k.classify("cog")
