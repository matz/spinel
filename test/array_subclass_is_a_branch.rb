# `if x.is_a?(Array)` on a local that holds an Array subclass takes the
# then arm, as the bare `x.is_a?(Array)` value does: the class is an Array,
# Enumerable and Object.

class SubA < Array; end
class Gives; def to_ary = SubA[7, 8]; end

x = SubA[1, 2]
p(if x.is_a?(Array) then :array else :other end)
p(if x.kind_of?(Array) then :array else :other end)
p(if x.is_a?(Enumerable) then :enum else :other end)
p(if x.is_a?(Object) then :obj else :other end)
p(if x.instance_of?(Array) then :exact else :sub end)
p(if x.instance_of?(SubA) then :exact else :sub end)
p(if x.is_a?(Hash) then :hash else :other end)
p(x.is_a?(Array) ? :array : :other)
puts "array" unless !x.is_a?(Array)

def kind(v)
  a = v.to_ary
  if a.is_a?(Array) then :arr elsif a.nil? then :nil else :other end
end
p kind(Gives.new)

class SubH < Hash; end
h = SubH.new
p(if h.is_a?(Hash) then :hash else :other end)
p(if h.is_a?(Enumerable) then :enum else :other end)
