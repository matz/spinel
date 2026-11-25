# A boxed receiver whose class answers transform_values or transform_keys
# through an attr reader, an alias, define_method or a Struct member calls
# it there: the Hash rewrite of the with_index form is left out in such a
# program.
class W
  def initialize(tag) = @tag = tag
  def with_index = [@tag]
end
class Reader
  attr_reader :transform_values
  def initialize = @transform_values = W.new(:reader)
end
class Defined
  define_method(:transform_values) { W.new(:defined) }
end
class Aliased
  def walk = W.new(:aliased)
  alias transform_keys walk
end
S = Struct.new(:transform_keys)

def order(k) = k.transform_values.with_index { |v, i| [v, i] }
def renamed(k) = k.transform_keys.with_index { |v, i| [v, i] }

[Reader.new, Defined.new].each { |k| p order(k) }
[Aliased.new, S.new(W.new(:member))].each { |k| p renamed(k) }
