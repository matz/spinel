# A module's own method lists read the visibility a body's word gives once it
# has run, each list the visibilities it shows: a word in a method body, and
# one in a module body a lambda runs, where the module's method has two defs;
# at the top level after the call that ran the word too, a class's list as well.
module M
  def m = :m
  def n = :n
  protected def q = :q
  def self.hide
    private :m
    public :q
    nil
  end
end
module L
  def m = :m
end
la = -> {
  module L
    def m = :m2
    private :m
  end
  nil
}
# a class's own list too, read at the top level after the call that ran the word
class C
  def a = :a
  def b = :b
  def self.hide = (private :a; nil)
end
def chk(t)
  p [t, M.private_instance_methods(false).include?(:m), M.instance_methods(false).include?(:m),
     M.public_instance_methods(false).include?(:m), M.protected_instance_methods(false).include?(:q),
     M.instance_methods(false).include?(:q), M.private_instance_methods(false).include?(:q)]
  p [t, L.private_instance_methods(false).include?(:m), L.public_method_defined?(:m)]
end
chk(:before)
M.hide
la.call
chk(:after)
C.hide
p [M.private_instance_methods(false).sort, M.public_instance_methods(false).sort, C.private_instance_methods(false), C.instance_methods(false).sort]
