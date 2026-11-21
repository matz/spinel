# A body's word or non-public def that no reader can follow yet keeps the
# form it has where the record holds no body versions: any word in a
# program that defines method_missing (also one on a module's method),
# and a non-public def in a body whose calls reach a send. Each still
# answers once its body ran, where that form gives CRuby's answer.
module M
  def q = :q
end
class K
  include M
  def m = :m
  def method_missing(n, *a) = n == :zz ? :mm : super
  def respond_to_missing?(n, p = false) = n == :zz || super
end
def look(k) = [(k.send(:m2) rescue $!.class), k.respond_to?(:m2, true)]
class K
  def self.setup
    private def m2 = :m2
    look(K.new)
  end
  def self.hide
    private :m
  end
end
pr = proc { module M; private :q; end; nil }
k = K.new
pr.call
K.hide
K.setup
p [k.respond_to?(:q), (k.q rescue $!.class), k.send(:q)]
p [k.send(:m), k.respond_to?(:m, true)]
p look(k)
# a word with an Array of names, one of them inherited, and one a class
# method may run on a subclass: the calls before it runs answer the
# methods as they are
class P0
  def pm = :pm
end
class Q0 < P0
  def qm = :qm
  def self.hide = private([:pm, :qm])
end
q = Q0.new
p [(q.pm rescue $!.class), (q.qm rescue $!.class)]
Q0.hide
class R0
  def rm = :rm
  def self.hide = private([:rm])
end
class S0 < R0; end
S0.hide
p((R0.new.rm rescue $!.class))
# a barred word's value is its argument, whether or not it applies: a
# class method's word on an inherited name a subclass's call runs it on
class U0
  def um = :um
  def self.hide = private(:um, :um)
end
class V0 < U0; end
p V0.hide
