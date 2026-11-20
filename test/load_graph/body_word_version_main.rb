# A visibility word a method body runs on the class self is there
# (`private :m` in a class method), and a non-public def such a body
# makes (`private def m2`), are versions of their methods: the readers
# and the calls before the body runs answer as CRuby does there, and the
# ones after it see the new visibility.
class K
  def m = :m
  def n = :n
  def o = :o
end
def chk(tag, k)
  xs = [k, 1]
  p [tag, (k.m rescue $!.class), k.respond_to?(:m), k.respond_to?(:m, true), K.public_method_defined?(:m), K.private_method_defined?(:m)]
  p [tag, (k.o rescue $!.class), K.protected_method_defined?(:o), xs.map { |x| x.respond_to?(:m) }, (k.n rescue $!.class)]
  p [tag, (k.m2 rescue $!.class), k.respond_to?(:m2), k.respond_to?(:m2, true), K.private_method_defined?(:m2), K.public_method_defined?(:m2)]
end
class K
  def self.hide
    private :m
    protected :o
    chk(:during, K.new)
  end
  def self.setup
    private def m2 = :m2
  end
end
k = K.new
chk(:before, k)
K.hide
chk(:after, k)
K.setup
chk(:setup, k)
# a word in a class body a proc runs
class H
  def baz = "baz"
end
def hchk(t)
  r = [(H.new.baz rescue $!.class), H.new.respond_to?(:baz)]
  r << begin; H.new.baz; rescue NoMethodError; :hidden; end
  r << begin; H.new.respond_to?(:baz); end
  p [t, r]
end
pr = proc do
  class H
    private :baz
  end
  nil
end
hchk(:before)
pr.call
hchk(:after)
# a word with one Array of names, and a protected method called in an
# instance_exec block inside one on an instance of its class
class J
  def ja = :ja
  def jb = :jb
  def pv = :pv
  def self.hide = private([:ja, :jb])
  def self.prot = protected(:pv)
end
j = J.new
J.hide
J.prot
p [(j.ja rescue $!.class), (j.jb rescue $!.class), (j.pv rescue $!.class)]
def jx(j) = [J.new.instance_exec { instance_exec { j.pv } }, J.new.instance_exec { self.instance_exec { j.pv } }]
p jx(j)
