# A program's own class method `allocate` answers the call, as any class
# method does: on a class, on a module (where Class#allocate would raise
# NoMethodError), through a singleton class, an inherited one, or a module
# the class extends. A class without one still allocates a bare instance,
# and a module without one still raises.
class K
  def self.mk = allocate
  def self.allocate = :k_alloc
end
p K.allocate
p K.mk
class L; def self.allocate = :l_alloc; end
class L2 < L; end
p L2.allocate
module M; def self.allocate = :m_alloc; end
p((M.allocate rescue :rescued))
module N; end
class << N; def allocate = :n_alloc; end
p((N.allocate rescue :rescued))
module W; def allocate = :w_alloc; end
class Z; extend W; end
p Z.allocate
class Er < StandardError; def self.allocate = :er_alloc; end
p Er.allocate
class Q; def initialize = @q = 1; end
p Q.allocate.class
module P; end
p((P.allocate rescue :rescued))
class Er2 < StandardError; end
p Er2.allocate.class
