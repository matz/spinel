# A class-side alias of a method of a module the parent extends, whose body
# calls super into another extended module, starts that super where the
# original's does, before and after the module defines the name again.
module Base
  def m = [:base]
end
module M
  def m = [:m1, *super]
end
class A
  extend Base
  extend M
end
class B < A
  class << self
    alias n m
  end
end
p B.n, B.m
module M
  def m = [:m2]
end
p B.n, B.m
