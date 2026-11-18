# a bare call and a receiver call of a name a body defines later, and of a
# placed late def: NameError for the bare call, NoMethodError for the receiver call, before;
# the method after. Two calls in one statement, a tail call, three levels, a nil-able receiver.
def err(e = $!) = e.is_a?(NameError) ? [e.class, e.name] : e.class
class K
  def self.callm = K.m
  def self.callv = m
  def self.tail
    return m
  end
  def self.lvl1 = lvl2
  def self.lvl2 = lvl3
  def self.lvl3 = m
  def self.nilr(k) = k&.m
  L = lambda { |n|
    class << self
      def m = 22
    end
  }
end
class P
  def self.callm = P.w
  def self.callv = w
end
p [(K.callm rescue err), (K.callv rescue err)]
p (K.tail rescue err), (K.lvl1 rescue err), (K.nilr(K) rescue err), K.nilr(nil)
p [(P.callm rescue err), (P.callv rescue err)]
class P
  def self.w = 33
end
K::L.call(1)
p [K.callm, K.callv], K.tail, K.lvl1, K.nilr(K)
p [P.callm, P.callv]

# A body under a run-time condition defines names only a class's
# instances answer as builtins: a class-side call raises before it runs.
class U
  def self.setup
    def self.size = 1
    def self.first = 3
    def self.count = 6
  end
end
U.setup if ARGV.size > 5
p [(U.size rescue $!.class), (U.first rescue $!.class), (U.count rescue $!.class)]
U.setup
p [U.size, U.first, U.count]
