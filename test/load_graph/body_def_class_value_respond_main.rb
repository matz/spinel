# A respond_to? on a value that may be a class answers each class's arm
# from the version cells of a class method a class method's body defines,
# as a call of it there finds it: false until the body has run.
class K
  def self.setup = (def self.a = :a)
end
class L
  def self.a = :la
end
class S < K; end
def show(tag, ys)
  p [tag, ys.map { |y| y.respond_to?(:a) }, ys.map { |y| y.respond_to?(:a, true) }, ys.map { |y| (y.a rescue $!.class) }]
end
ys = [K, L, S, 1, nil, K.new]
p [(K.a rescue $!.class), K.respond_to?(:a), ys.map { |y| y.respond_to?(:a) }]
show(:before, ys)
K.setup
show(:after, ys)
p [(K.a rescue $!.class), K.respond_to?(:a), ys.map { |y| y.respond_to?(:a) }]
f = ->(y) { y.respond_to?(:a) }
p [f.(K), f.(1)]

# A placed class method and a body's: the class answers throughout.
class R
  def self.a = :old
  def self.setup = (def self.a = :new)
end
def rshow(tag, ys) = p([tag, ys.map { |y| y.respond_to?(:a) }, ys.map { |y| (y.a rescue $!.class) }])
rs = [R, 2]
rshow(:a, rs)
R.setup
rshow(:b, rs)

# A module's instance method made by a body reaches the class that
# extends it at the same time.
module M
  def self.setup = (def q = :q)
end
class E; extend M; end
class F; end
def eshow(es) = p([es.map { |y| y.respond_to?(:q) }, (E.q rescue $!.class), E.respond_to?(:q)])
es = [E, F, M]
eshow(es)
M.setup
eshow(es)
