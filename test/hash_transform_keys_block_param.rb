# transform_keys with a block a parameter hands on (`&block`): the block has
# no body at the call, so what its keys are is unknown there and the Hash it
# answers holds boxed keys. The receiver's own key kind (String) stored the
# Symbol keys the block answers, and the C did not compile.
def keys_of(h, &block) = h.transform_keys(&block)
def keys_of!(h, &block) = h.transform_keys!(&block)

class Headers < Hash
  def keys_to(&block) = dup.transform_keys!(&block)
end

h = { "a" => "1", "b" => "2" }
p keys_of(h, &:to_sym)
g = { "a" => "1" }
p keys_of!(g, &:to_sym)
p g
p keys_of(h) { |k| k.upcase }
p keys_of({ 1 => :x }) { |k| k * 2 }

s = Headers.new
s["a"] = "1"
t = s.keys_to(&:to_sym)
p t, t.class
