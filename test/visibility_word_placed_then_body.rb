# A visibility word placed in a class body, then a class method's body that
# runs a word of the same name later: the body's word is a version ordered
# after the placed one, so the name has the placed word's visibility before
# the body runs and the body's after it, for a call, respond_to? and the
# method lists, in a class and through a subclass. A word in a rescued begin
# is a placed word too.
class K
  def a = :a
  def b = :b
  def c = :c
  private :a
  begin
    private :b
  rescue NameError
  end
  protected :c
  def self.unhide = public(:a, :b)
  def self.hide = private(:c)
end
class S < K; end
def show(tag)
  k = K.new
  p [tag, (k.a rescue $!.class), (k.b rescue $!.class), (k.c rescue $!.class),
     k.respond_to?(:a), K.private_instance_methods(false).sort, K.protected_instance_methods(false).sort,
     (S.new.a rescue $!.class)]
end
show(:before)
K.unhide
show(:after_unhide)
K.hide
show(:after_hide)
