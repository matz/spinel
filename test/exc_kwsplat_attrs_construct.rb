# A KeyError, NameError, NoMethodError or FrozenError built with a `**hash` records the key and the
# receiver the hash carries, as it does with literal keywords (K-061: the call recorded neither and
# `#key` raised "no key is available"). Unknown keywords are still ArgumentError.
kw = {receiver: 3, key: :q}
x = KeyError.new("a", **kw)
p x.message, x.key, x.receiver
kw2 = {key: :only}
y = KeyError.new("b", **kw2)
p y.message, y.key
p (y.receiver rescue "no recv")
z = KeyError.new("c", **{key: :lit})
p z.key
n = NameError.new("n", :f, **{receiver: 9})
p n.name, n.receiver
nm = NoMethodError.new("m", :g, [1], **{receiver: 8})
p nm.receiver, nm.args
f = FrozenError.new("fz", **{receiver: "r"})
p f.receiver
def mk(**o) = KeyError.new("d", **o)
w = mk(key: :dyn, receiver: {})
p w.key, w.receiver
v = mk
p (v.key rescue "no key")
h = {key: :hk}
p KeyError.new("e", receiver: 1, **h).key

class MyKey < KeyError
  def initialize(msg, **o)
    super(msg, **o)
  end
end
class PlainKey < KeyError; end
o = {key: :sub, receiver: [1]}
a = MyKey.new("m", **o)
p a.message, a.key, a.receiver
b = PlainKey.new("pk", **o)
p b.message, b.key, b.receiver
begin; raise KeyError.new("r", **o); rescue KeyError => e; p e.message, e.key, e.receiver; end
e0 = KeyError.new("empty", **{})
p e0.message
p (e0.key rescue "no key")
begin
  bad = {foo: 1}
  KeyError.new("u", **bad)
rescue ArgumentError => e
  p e.class, e.message
end
r = {receiver: :rr}
p NameError.new("nn", :nm, **r).receiver
p FrozenError.new("fz", **r).receiver
p NoMethodError.new("nm", :zz, [2], **r).receiver
