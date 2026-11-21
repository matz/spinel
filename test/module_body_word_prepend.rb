# A visibility word a method body runs on a module's method acts on the
# copies a prepend puts in front of a class's methods: on a prepend's own
# layer, on one that comes after the class's def, and on the copy a
# reopening's include makes after the word ran.
module P
  def q = [:p, super]
  def self.hide_q = (private :q; nil)
end
class K; prepend P; def q = :kq; end
def chk(t, o) = p([t, (o.q rescue $!.class), o.respond_to?(:q), o.respond_to?(:q, true), (o.send(:q) rescue $!.class)])
chk(:before, K.new); P.hide_q; chk(:after, K.new)

module P2
  def r = :r
  def self.hide_r = (private :r; nil)
end
class K2; def r = :kr; end
class K2; prepend P2; end
def chk2(t, o) = p([t, (o.r rescue $!.class), o.respond_to?(:r), o.respond_to?(:r, true), (o.send(:r) rescue $!.class)])
chk2(:before, K2.new); P2.hide_r; chk2(:after, K2.new)

# a word run from a proc, read by private_method_defined? too
class K3; def m = :m; end
module P3; def m = [:pm, super]; end
class K3; prepend P3; end
def chk3(t, k) = p([t, (k.m rescue $!.class), k.respond_to?(:m), K3.private_method_defined?(:m), [k, 1].map { |x| x.respond_to?(:m) }])
k3 = K3.new
pr = proc { module P3; private :m; end; nil }
chk3(:before, k3); pr.call; chk3(:after, k3)

# a reopening's include after the word ran
module P4
  def s = :s
  def self.hide_s = (private :s; nil)
end
class K4; end
P4.hide_s
class K4; include P4; end
p [(K4.new.s rescue $!.class), K4.new.respond_to?(:s), K4.new.respond_to?(:s, true), K4.new.send(:s)]
