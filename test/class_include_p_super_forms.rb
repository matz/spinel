# A class method's `super` into Module#include? from a rest, a plain
# parameter or a module's own method answers a Boolean: the receiving
# class's, with CRuby's ArgumentError for a count it does not take and its
# TypeError naming what was passed in place of a module.
module M; end
module N; include M; end
class K
  include N
  def self.include?(*a) = super
end
class K2 < K; end
class K3 < K2; include Comparable; end
class L
  def self.include?(m) = super
end
module Q
  include M
  def self.include?(*a) = super
end
def t(k, m) = k.include?(m)
p K.include?(M), K.include?(N), K2.include?(M), K3.include?(Comparable), K2.include?(Comparable)
p Q.include?(M), Q.include?(Kernel)
b = [M, Comparable, nil][ARGV.size + 1]
p K.include?(b) rescue p $!.class
p t(K, M), t(K3, Enumerable)
p K.include?(*[M])
p(K.include?(M) { 1 })
begin; K.include?(M, k: 1); rescue ArgumentError => e; p e.message; end
begin; K.include?(nil); rescue TypeError => e; p e.message; end
begin; K.include?(K2); rescue TypeError => e; p e.message; end
begin; L.include?(String); rescue TypeError => e; p e.message; end
p L.include?(Kernel) && K.include?(M)
x = [K, K2].map { |k| k.include?(N) }
p x
def u = K.include?(M) ? :y : :n
p u
nb = [M, nil, 1, true, K][ARGV.size + 1]
[nb, nil, 1, true, K, M].each { |v| begin; p K.include?(v); rescue TypeError => e; p e.message; end }
[nb, 1, K, M].each { |v| begin; p L.include?(v); rescue TypeError => e; p e.message; end }
begin; K.include?; rescue ArgumentError => e; p e.message; end
begin; Q.include?(M, N); rescue ArgumentError => e; p e.message; end
