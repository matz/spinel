# A protected word a method body runs on a module's method closes it to a
# caller whose class neither includes nor prepends the module, and leaves
# it open to one that does (the caller's self is kind_of? the module); a
# class's own def in front of a module's method keeps the class's
# visibility, whatever the module's copy behind it holds.
module M
  def q = :q
  def self.hide_q = (protected :q; nil)
end
class K; include M; def via(o) = o.q; end
class J; include M; def via(o) = o.q; end
class X; def via(o) = o.q; end
class P; prepend M; def via(o) = o.q; end
class PS < P; end
module N; def n = :mn; end
class KN; include N; def n = :kn; def self.hide_n = (private :n; nil); end
def chk(t)
  k = K.new
  p [t, (k.q rescue $!.class), K.new.via(k), (J.new.via(k) rescue $!.class), (X.new.via(k) rescue $!.class),
     k.respond_to?(:q), k.respond_to?(:q, true), M.protected_method_defined?(:q)]
  p [t, P.new.via(k), PS.new.via(k), P.new.via(PS.new), (X.new.via(P.new) rescue $!.class)]
  o = KN.new
  p [(o.n rescue $!.class), o.respond_to?(:n), o.respond_to?(:n, true), (o.send(:n) rescue $!.class)]
end
chk(:before); M.hide_q; KN.hide_n; chk(:after)
