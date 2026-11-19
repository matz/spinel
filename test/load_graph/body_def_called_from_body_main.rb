# A class method's body that defines a method and calls a program method
# reading it runs code the load record follows: the def is the body's
# version, so the reader's calls before the body has run raise
# NoMethodError, and respond_to? answers false, as in CRuby.
class K
  def self.setup
    chk(:in0, K.new)
    def m2(a = 1, *r, k: 2) = [:m2, a, r, k]
    chk(:in1, K.new)
  end
  def self.run = p([:run, K.new.respond_to?(:m2), (K.new.m2 rescue $!.class)])
end
def chk2(k) = (k.m2(5, 6, k: 7) rescue $!.class)
def chk(tag, k)
  p [tag, chk2(k), (k.m2 rescue $!.class), [k, nil, 1].map { |x| (x.m2 rescue $!.class) }, k.respond_to?(:m2)]
end
chk(:before, K.new)
K.run
K.setup
chk(:after, K.new)
K.run

# A body that also calls a method whose own body defines: that one is
# followed too, the record's version of each name opens when its body runs.
class L
  def self.setup
    def l2 = :l2
    helper
  end
  def self.helper
    def l4 = :l4
    nil
  end
end
def lchk = p([(L.new.l2 rescue $!.class), (L.new.l4 rescue $!.class)])
lchk
L.setup
lchk

# A later class's placed def and body def of a name such a reader also
# calls: the later body def is still that body's version, so the placed
# one answers until its body has run.
class Q
  def self.setup = (def m3 = :q)
end
def qchk = p((Q.new.m3 rescue $!.class))
qchk
Q.setup
qchk
class U
  def self.setup = (def u = :u)
end
class R
  def m3 = :old
  def self.setup = (def m3 = :new)
end
def rchk = p(R.new.m3)
rchk
R.setup
rchk
