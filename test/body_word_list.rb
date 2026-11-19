# A list of a class's own instance methods of one visibility reads the
# visibility a body's word gives a method where the list runs: the word's
# cells decide, not the visibility the method ends with.
class K
  def m = :m
  def n = :n
end
k = K.new
pr = proc { class K; private :m; end; nil }
def lists(t) = p([t, K.public_instance_methods(false).include?(:m), K.private_instance_methods(false).include?(:m), K.public_instance_methods(false).include?(:n)])
p [k.respond_to?(:m), K.instance_method(:m).owner]
lists(:before)
pr.call
p [k.respond_to?(:m)]
lists(:after)
