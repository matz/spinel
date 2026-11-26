# a subclass of Hash or Array named through a constant that holds the
# builtin (Concurrent::Hash's shape)
HashImpl = ::Hash
class MyHash < HashImpl
end

h = MyHash.new
h[:a] ||= 1
h[:b] = 2
p h, h.size, h.keys, h.class, h.is_a?(Hash)

# a two-step alias chain
Inner = ::Hash
Outer = Inner
class Deep < Outer
  def extra = size * 10
end

d = Deep.new
d[:x] = 1
d[:y] ||= 2
p d, d.extra, d.is_a?(Hash), Deep.ancestors.include?(Hash), Deep.superclass

ArrImpl = ::Array
class MyArr < ArrImpl; end

a = MyArr.new
a << 3
p a, a.is_a?(Array), a.class

# the alias named from the top level
RootImpl = ::Hash
class RootHash < ::RootImpl; end

rh = RootHash.new
rh[:r] = 1
p rh, rh.is_a?(Hash), RootHash.superclass

# an alias of another module's constant named Hash is that constant
module Other
  Hash = ::Object
end
OtherImpl = Other::Hash
class NotHash < OtherImpl; end

p NotHash.superclass, NotHash.new.is_a?(Hash)
