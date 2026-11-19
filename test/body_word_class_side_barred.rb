# A class-side word or non-public def whose name a reader that cannot
# follow the class side reads keeps the form it has where the record holds
# no body versions: a boxed Class value's respond_to? or call, a defined?
# of the call, a class-side list. The reader and the call it guards then
# agree: the guarded call does not raise.
class K; end
ys = [K, 1, nil]
pr = proc do
  class K
    class << self
      def m = :s
      private :m
    end
  end
  nil
end
pr.call
ys.each { |x| x.m if x.respond_to?(:m) }
p [K.respond_to?(:m, true), ys.map { |x| x.respond_to?(:m, true) }]

class L
  def self.n = :n
  def self.hide
    class << self
      private :n
    end
  end
end
class L2 < L; end
zs = [L, L2, 1]
L.hide
zs.each { |x| x.n if x.respond_to?(:n) }
p zs.map { |x| x.respond_to?(:n, true) }

class P
  def self.o = :o
  def self.q = :q
end
hp = proc { class P; class << self; private :o, :q; end; end; nil }
hp.call
P.o if defined?(P.o)
P.q if P.singleton_methods.include?(:q)
p [P.respond_to?(:o, true), P.respond_to?(:q, true)]
