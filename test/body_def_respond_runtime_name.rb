# A respond_to? of a name only the run knows answers a method a class method's body
# defines by its version cell, as a literal respond_to? of it does: false until the
# body has run, whatever the receiver (one class or a boxed value of several), and the
# guard it makes agrees with the call.
NM = [:m, :x][ARGV.size]
class K
  def self.setup
    def m = :k
  end
end
class J < K; end
class P
  def m = :p
end

p [K.new.respond_to?(NM), K.new.respond_to?(NM, true), (K.new.m rescue $!.class), K.new.respond_to?(:m)]
xs = [K.new, J.new, P.new]
guard = proc { xs.map { |x| x.respond_to?(NM) ? x.m : :none } }
p guard.call
K.setup
p [K.new.respond_to?(NM), K.new.respond_to?(NM, true), K.new.m, K.new.respond_to?(:m)]
p guard.call

# the guard in tail position, called before and after
class T
  def self.setup = (def t = :t)
end
TN = :t
def tail = T.new.respond_to?(TN) ? T.new.t : :none
p tail
T.setup
p tail

# a private def answers only to include_all, and a name nothing defines is false
class V
  def self.setup
    private def v = :v
  end
end
VN = [:v, :w][ARGV.size]
p [V.new.respond_to?(VN), V.new.respond_to?(VN, true), V.new.respond_to?(:w)]
V.setup
p [V.new.respond_to?(VN), V.new.respond_to?(VN, true), V.new.respond_to?(:w)]
