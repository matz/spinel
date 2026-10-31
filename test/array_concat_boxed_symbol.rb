# spinel: gc-stress
# This boxed PolyArray contains a Symbol; it does not use the Symbol Array
# tag. test/concat-repr/symbol_bridge.rb covers that native representation.
# The general receiver and its identity survive either result use.
x = [[:s], 0].first
a = [1, "s"]
a.concat(x)
p a
p a.last.class
a = [1, "s"]
r = a.concat(x)
p r
p r.equal?(a)
a = [1, "s"]
a.concat([2], x)
p a
p a.last.class
a = [1, "s"]
r = a.concat([2], x)
p r
p r.equal?(a)

# A later boxed conversion requires capture of the first operand.
class EmptyConcatSource
  def to_ary
    []
  end
end
y = [EmptyConcatSource.new, 0].first
a = [1, "s"]
p a.concat(x, y)
p a.last.class
