# A hook's alias of Hash#each in a subclass that overrides each and
# passes a block through the alias.
module Raw
  def self.included(base)
    base.class_eval do
      alias_method :raw_each, :each
    end
  end
end
class Bag < Hash
  include Raw
  def each
    raw_each { |k, v| yield k.to_s, v + 1 }
  end
end
b = Bag.new
b[:x] = 1
b[:y] = 2
out = []
b.each { |k, v| out << [k, v] }
p out
