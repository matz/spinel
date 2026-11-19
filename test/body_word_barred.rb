# A body's word or non-public def that no reader can follow yet keeps the
# form it has where the record holds no body versions: a word on a
# module's method (its include copies carry no word versions), any word
# in a program that defines method_missing, and a non-public def in a
# body whose calls reach a send. Each still answers once its body ran,
# where that form gives CRuby's answer.
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
