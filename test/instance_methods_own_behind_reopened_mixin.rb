# Module#instance_methods(false) of a class that mixes a module in from a
# reopening: the mixin's method is the module's, and the class lists it
# only where it has a body of its own under the name (#12224).
class P
  module N
    def b? = true
  end
end
class P
  include P::N
  def k = 3
end
module M; def b? = :m; end
class Q
  def b? = :own
end
class Q
  include M
end
class R
  def b? = :own
end
class R
  prepend M
end
class S2; end
class S2
  prepend M
end
p P.new.b?, Q.new.b?, R.new.b?, S2.new.b?
p P.instance_methods(false), Q.instance_methods(false), R.instance_methods(false), S2.instance_methods(false)
p P.public_instance_methods(false), P.method_defined?(:b?), P.instance_methods.include?(:b?)
p P.new.respond_to?(:b?), P.new.method(:b?).owner
