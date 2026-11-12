# An alias takes the visibility of the method it captured: a builtin's private
# method (Kernel#puts) makes a private alias, a builtin's public one a public
# alias, whatever section the alias statement stands in; an alias of a method
# of a program class (below a parent's) and of a method that was private when
# aliased follow the original at the alias.

class K
  alias pp2 puts
  alias_method :pp3, :puts
  alias pr2 print
  alias_method :ob, :object_id
  alias ins inspect
  def own = 1
  alias own2 own
  private :own
  alias own3 own
end
p K.instance_methods(false).sort
p K.private_instance_methods(false).sort
p [K.method_defined?(:pp2, false), K.private_method_defined?(:pp2, false), K.private_method_defined?(:pp2)]
p [K.method_defined?(:ob, false), K.private_method_defined?(:ob, false)]
p [K.private_method_defined?(:own3, false), K.private_method_defined?(:own2, false)]

class Pa
  def foo = 1
  private :foo
  def bar = 2
  protected :bar
end
class Sub < Pa
  alias foo2 foo
  alias_method :bar2, :bar
  alias baz puts
end
p Sub.instance_methods(false).sort
p Sub.private_instance_methods(false).sort
p Sub.protected_instance_methods(false).sort

class Section
  alias pp2 puts
  private
  alias pp3 inspect
  public
  alias pp4 puts
end
p Section.instance_methods(false).sort
p Section.private_instance_methods(false).sort

class Kernels
  alias rr require
  alias_method :ra, :raise
  alias_method :ss, :sleep
  alias_method :ap, :p
end
p Kernels.private_instance_methods(false).sort
p Kernels.instance_methods(false).sort

class Object
  alias pp9 puts
end
p Object.private_instance_methods(false).include?(:pp9)
p Object.instance_methods(false).include?(:pp9)
