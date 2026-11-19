# A boxed Class value's respond_to? counts its class method's visibility:
# the one where the reader runs, the class body's where it does not vary,
# and every version's where they agree; a `true` second argument counts
# the non-public ones too
class K
  def self.m = :s
  def self.q = :q
  def im = :i
end
class K2 < K; end
ys = [K, 1, nil, K.new, K2]
def rd(xs) = xs.map { |x| x.respond_to?(:m) }
p ys.map { |x| x.respond_to?(:m) }
p ys.map { |x| x.respond_to?(:m, true) }
p rd(ys)
class K
  private_class_method :m
  class << self
    protected :q
  end
end
p ys.map { |x| x.respond_to?(:m) }
p ys.map { |x| x.respond_to?(:m, true) }
p ys.map { |x| x.respond_to?(:q) }, ys.map { |x| x.respond_to?(:q, true) }
p [ys[0].respond_to?(:m), ys[4].respond_to?(:m)]
p ys.map { |x| [x.respond_to?(:m), x.respond_to?(:q, true)] }
class K
  public_class_method :m
end
p ys.map { |x| x.respond_to?(:m) }
p rd(ys)

# private from the class body on, read in a method too
class H
  def self.hm = :s
  private_class_method :hm
  def self.n = :n
  class << self
    protected def pq = :pq
  end
end
hs = [H, 1, nil]
p hs.map { |x| x.respond_to?(:hm) }, hs.map { |x| x.respond_to?(:hm, true) }, hs.map { |x| x.respond_to?(:n) }
p hs.map { |x| x.respond_to?(:pq) }, hs.map { |x| x.respond_to?(:pq, true) }
def via(x) = x.respond_to?(:hm)
p [via(H), via(1)]
