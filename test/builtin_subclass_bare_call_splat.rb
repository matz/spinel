# A receiverless call in a builtin subclass's method that passes a splat
# reaches the builtin's method on self, as the `self.` form does.

class Params < Hash
  alias_method :to_params_hash, :to_h

  def pairs(*r) = to_a(*r)
  def names(*r) = keys(*r)
end

class Row < Array
  def all(*r) = to_a(*r)
  def head(*r) = first(*r)
end

class Wrapper
  def initialize = (@hash = {"w" => 0})
  def to_h(&block) = @hash.dup
end

pa = Params.new
pa["k"] = 1
p pa.to_params_hash
p pa.pairs
p pa.names
p((pa.pairs(1) rescue $!.class))

row = Row.new
row << 1 << 2
p row.all
p row.head
p row.head(1)
p Wrapper.new.to_h

# a receiver that may be either class reaches the alias's forwarder
class Indifferent < Hash
end

class Parser
  def make_params = Params.new
end

class Builder
  def make_params = Indifferent.new
end

def expand(parser, pairs)
  params = parser.make_params
  pairs.each { |k, v| params[k] = v }
  params.to_params_hash
end

p expand(Parser.new, [["b", 2]])
p((expand(Builder.new, []) rescue $!.class))
