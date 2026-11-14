# A conditional respond_to_missing? in a class that shares its short name with
# a blank slate in another scope: the class that has the def is an Object
# subclass (Kernel's row answers false where the branch did not run), the
# blank slate is P::Q. A name two scopes carry is not known to lack Kernel, so
# the def keeps the answer it had. Z reaches P::Base, a plain class.
class Base < BasicObject; end
class Q
  if ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
  def t = respond_to_missing?(:x, false)
end
module P
  class Q < BasicObject; end
  class Base; end
  class Z < Base
    def t = respond_to_missing?(:x, false)
  end
end
p Q.new.t
p P::Z.new.t
