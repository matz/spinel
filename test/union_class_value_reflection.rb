# A boxed value that can be a class or another value answers, when it is
# the class, what the class answers called directly: its own and inherited
# instance-method lists, for a class or a module, beside an Integer, nil or
# a String, read from a local, an Array, a Hash or a method parameter, and
# beside a class method of the same name another class defines. The class
# raised NoMethodError, as if it were the other value. An Integer or nil in
# the same slot still raises CRuby's NoMethodError.
class W; def w = 0; end
class X < W
  def a = 1
  private def b = 2
  protected def pr = 4
end
class Y; def c = 5; end
class Z; def self.instance_methods(x) = [:z]; end
module M; def m = 1; end

c = rand < 2 ? X : 5
p c.instance_methods(false).sort
p c.public_instance_methods(false)
p c.private_instance_methods(false)
p c.protected_instance_methods(false)
p c.instance_methods.include?(:w)
p c.public_instance_methods(true).include?(:w)

m = rand < 2 ? M : 5
p m.instance_methods(false)

n = rand < 2 ? X : nil
p n.instance_methods(false).size
s = rand < 2 ? X : "s"
p s.instance_methods(false).map(&:to_s).sort

[X, Y].each { |k| p k.instance_methods(false).sort }
h = { k: X, n: 1 }
p h[:k].protected_instance_methods(false)

def own_count(k) = k.instance_methods(false).size
p own_count(X)

i = rand < 0 ? X : 5
[-> { i.instance_methods(false) }, -> { i.name }].each do |f|
  f.call
rescue NoMethodError => e
  puts e.message
end
z = rand < 0 ? X : nil
begin
  z.instance_methods(false)
rescue NoMethodError => e
  puts e.message
end
begin
  own_count(7)
rescue NoMethodError => e
  puts e.message
end
