# An object's own singleton method is listed by `public_methods(false)` whether or not the class has run
# the body that defines a method of the same name: the class's cell says nothing of the object's own def.
class K
  def self.setup = (def m = :k)
  def n = 1
end
k = K.new
def k.m = :s
q = K.new
class J; def j = 1; end
xs = [k, q, J.new]
p xs.map { |x| x.public_methods(false).include?(:m) }
p [k.public_methods(false).include?(:m), (K.setup; q.public_methods(false).include?(:m)), k.public_methods(false).include?(:m)]
p xs.map { |x| x.public_methods(false).include?(:m) }
l = ->(o) { o.public_methods(false).include?(:m) }
p [l.call(k), l.call(q)]
fl = ARGV.empty?
p [k.public_methods(false).include?(:m), k.public_methods(fl).include?(:m), k.methods(false).include?(:m), k.singleton_methods.include?(:m)]

# a singleton method that is private hides the class's public one
class P
  def self.setup = (def pm = :p)
end
pk = P.new
class << pk
  private def pm = :s
end
pq = P.new
p [pk.public_methods(false).include?(:pm), pq.public_methods(false).include?(:pm)]
P.setup
p [pk.public_methods(false).include?(:pm), pq.public_methods(false).include?(:pm)]
