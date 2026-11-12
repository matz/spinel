# A method list asks the call plan's own-entry question for each name it holds:
# a class method made private below its definer, a private `class << self`
# accessor, class methods the program names with two underscores (and one it
# names as the instance-variable accessors the compiler makes), a method a
# prepend displaced, with several prepends and an alias, and a method
# `private` makes after a prepend (the lists of them).

class A
  def self.x; 1; end
  def self.z; 3; end
end
class B < A
  private_class_method :x
  def self.y; 2; end
end
class C < A
  def self.z; 4; end
  private_class_method :z
end
p A.singleton_methods.sort
p B.singleton_methods.sort
p B.singleton_methods(false).sort
p C.singleton_methods.sort
p C.singleton_methods(false).sort

class Acc
  class << self
    attr_accessor :a
    attr_reader :b
    private :a
    private :b
  end
end
class AccSub < Acc
end
p Acc.singleton_methods(false).sort
p Acc.singleton_methods.sort
p AccSub.singleton_methods.sort
p Acc.respond_to?(:a)

class Dun
  def self.__x; 1; end
  def self.__y__; 2; end
  def self.y; 3; end
  class << self
    def __z; 4; end
    alias __w __z
  end
  def __i; 5; end
  def __a; 6; end
  private def __c; 7; end
  alias __d __a
  @q = 5
  class << self
    attr_accessor :q
  end
end
class DunSub < Dun
end
p Dun.singleton_methods(false).sort
p DunSub.singleton_methods.sort
p Dun.instance_methods(false).sort
p Dun.private_instance_methods(false).sort
p Dun.__x

class UserCiv
  def self.__spinel_civget_q; 2; end
  def self.y; 3; end
end
p UserCiv.respond_to?(:__spinel_civget_q)
p UserCiv.singleton_methods(false).sort

module M
  def x; "M" + super; end
  def z; "Mz" + super; end
end
module N
  def x; "N" + super; end
end
class TwoPrepends
  def x; "C"; end
  def z; "Cz"; end
  protected :z
  alias y x
  prepend M
  prepend N
end
o = TwoPrepends.new
p o.x
p o.y
p TwoPrepends.instance_methods(false).sort
p TwoPrepends.protected_instance_methods(false).sort
p TwoPrepends.public_instance_methods(false).sort

module PM
  def x; super; end
end
class PrivAfter
  def x; 1; end
  def w; 2; end
  prepend PM
  private :x
end
class PrivBefore
  prepend PM
  def x; 1; end
  private :x
end
p PrivAfter.instance_methods(false).sort
p PrivAfter.private_instance_methods(false).sort
p PrivBefore.instance_methods(false).sort
p PrivBefore.private_instance_methods(false).sort

class CCls
  def self.a; 1; end
  class << self
    alias b a
    alias_method :c, :a
    private :b
  end
  private_class_method :a
end
class DCls < CCls
  class << self
    public :a
  end
end
p CCls.singleton_methods(false).sort
p CCls.singleton_methods.sort
p DCls.singleton_methods.sort
