# A hook that writes the module's own instance variable and also makes
# something of the class that includes it: the ivar is the module's (self
# is the module in the hook), and the extend, the class_eval block and the
# attr_accessor reach the includer.

module IvReg
  @hosts = []
  module CM
    def kind = "cm"
  end
  def self.included(base)
    @hosts << base.name
    base.extend(CM)
  end
  def self.hosts = @hosts
end
class IvA; include IvReg; end
class IvB; include IvReg; end
p IvReg.hosts, IvA.kind, IvB.kind

module IvEval
  @hosts = []
  def self.included(base)
    @hosts << base.name
    base.class_eval do
      def hello = "hello"
    end
  end
  def self.hosts = @hosts
end
class IvC; include IvEval; end
class IvD; include IvEval; end
p IvEval.hosts, IvC.new.hello, IvD.new.hello

module IvCnt
  @n = 0
  def self.included(base)
    @n += 1
    base.attr_accessor :v
  end
  def self.n = @n
end
class IvE; include IvCnt; end
class IvF; include IvCnt; end
e = IvE.new
e.v = 3
p IvCnt.n, e.v

# one that only counts: nothing of the includer is made
module IvOnly
  @seen = []
  def self.included(base)
    @seen << base.name
    @last = base
  end
  def self.seen = @seen
  def self.last = @last
end
class IvG; include IvOnly; end
class IvH; include IvOnly; end
p IvOnly.seen, IvOnly.last
