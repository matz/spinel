# A visibility word a method body runs on a module's method acts on every
# copy of it: the classes that include or extend the module answer the
# visibility the word gives once it has run, and the module's own presence
# readers do too. Before it runs, the method keeps its def's visibility.
module M
  def q = :q
  def self.hide_q = (private :q; nil)
end
class K; include M; end
class J; include M; def q = [:j, super]; end
# an extend copy
module E
  def e = :e
  def self.hide_e = (private :e; nil)
end
class KE; extend E; end
# a word in a module the module includes, and one that makes it public
module N
  def n = :n
  def self.hide_n = (protected :n; nil)
  def self.show_n = (public :n; nil)
end
module MN; include N; end
class KN; include MN; end
def chk(t)
  k = K.new
  p [t, (k.q rescue $!.class), k.respond_to?(:q), k.respond_to?(:q, true), (k.send(:q) rescue $!.class),
     (k.public_send(:q) rescue $!.class), J.new.q]
  p [K.private_method_defined?(:q), K.public_method_defined?(:q), M.private_method_defined?(:q), M.public_method_defined?(:q)]
  p [(KE.e rescue $!.class), KE.respond_to?(:e), [KE, 1].map { |y| y.respond_to?(:e) }]
  p [(KN.new.n rescue $!.class), MN.protected_method_defined?(:n), N.public_method_defined?(:n)]
end
chk(:before); M.hide_q; E.hide_e; N.hide_n; chk(:after); N.show_n; chk(:shown)
p [K.private_method_defined?(:q), M.method_defined?(:q)]
# a class that includes the module after the word ran
class KL; include E; end
p [(KL.new.e rescue $!.class), KL.new.respond_to?(:e, true), KL.new.send(:e)]
