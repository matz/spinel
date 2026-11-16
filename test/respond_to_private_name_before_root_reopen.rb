class K; end
o = K.new
p o.respond_to?(:puts, true)
p o.respond_to?(:puts)
class Object; def puts(*a) = super; end
p o.respond_to?(:puts, true)
p o.respond_to?(:puts)
