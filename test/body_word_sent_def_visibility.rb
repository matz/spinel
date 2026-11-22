# A literal send of a visibility word that takes a def in a class
# method's body, `send(:private, def m = :m)`, gives the def the word's
# visibility once the body has run, as `private def m = :m` does.
class K
  def self.setup
    send(:private, def m = :m)
    __send__(:protected, def pr = :pr)
    nil
  end
  def call_m = m
  def peer(o) = o.pr
end
# The def is taken only where self is a class or module (an instance method's
# `send(:private, ...)` raises and the def stays public), on self through an
# explicit receiver too, and a call before the body ran finds no method; a
# define_method block's self is an instance as well.
class J
  def setup
    send(:private, def m = :m)
  rescue NoMethodError => e
    p e.message
  end
  def self.setup2
    self.send(:private, def a = :a)
    self.private def bb = :bb
    nil
  end
end
class D
  define_method(:setup) do
    send(:private, def m = :m)
  rescue NoMethodError => e
    p e.class
  end
end
k = K.new
p [(k.m rescue $!.class), k.respond_to?(:m), (k.pr rescue $!.class)]
K.setup
p [(k.m rescue $!.class), k.respond_to?(:m), k.respond_to?(:m, true), k.call_m]
p [(k.pr rescue $!.class), k.respond_to?(:pr), K.new.peer(k)]
p [K.private_instance_methods(false).sort, K.protected_instance_methods(false)]
x = [k, 1][0]
p [(x.m rescue $!.class), x.respond_to?(:m), (x.pr rescue $!.class)]
j = J.new
j.setup
p [j.m, j.respond_to?(:m)]
p [(j.a rescue $!.message), (j.b rescue $!.message)]
J.setup2
p [(j.a rescue $!.message), (j.b rescue $!.message), j.respond_to?(:a), J.private_instance_methods(false).sort]
d = D.new
d.setup
p [d.m, d.respond_to?(:m)]
