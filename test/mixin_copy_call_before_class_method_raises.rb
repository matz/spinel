# A method an include copies into a class calls a class method defined only
# later: a call of the copy before the def raises NoMethodError, and one after
# it runs the def, as the module's own method would.
class K; end
module M
  def go(x) = K.rec(x)
end
class Foo
  include M
end
begin
  Foo.new.go(1)
rescue NoMethodError => e
  p e.class
end
class K
  def self.rec(x) = [:k, x]
end
p Foo.new.go(2)
