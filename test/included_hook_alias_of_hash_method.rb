# A hook that aliases a builtin's method in a subclass of Hash, which then
# defines the name itself and reaches the builtin through the alias.
module Indifferent
  def self.included(base)
    base.class_eval do
      alias_method :regular_writer, :[]=
      alias_method :regular_reader, :[]
      alias_method :plain_size, :size
    end
  end
end
class Mash < Hash
  include Indifferent
  def []=(k, v)
    regular_writer(k.to_s, v)
  end
  def [](k)
    regular_reader(k.to_s)
  end
end
m = Mash.new
m[:a] = 1
m["b"] = 2
p m[:a], m["a"], m[:b], m.plain_size, m.keys
