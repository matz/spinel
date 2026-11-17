class K; end
p K.respond_to?(:foo)
class K
  def self.foo = 1
end
p K.respond_to?(:foo)
z = [(class Z; def self.foo = 2; end; 0)]
p Z.foo
