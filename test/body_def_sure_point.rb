# A class method defined in a body that runs once (`R.setup`) is called directly after
# that statement and raises before it; with a later def in between, the calls read the cell.
class K
  def self.setup
    def self.m = 1
  end
end
begin; p K.m; rescue NoMethodError => e; p e.class; end
K.setup
p K.m
class K
  def self.m = 2
end
p K.m
K.setup
p K.m
class R
  def self.setup
    def self.n = :once
  end
end
begin; p R.n; rescue NoMethodError => e; p e.class; end
R.setup
p R.n
def again = R.n
p again
# A def in the same statement that runs after the call that runs the body (or before it) is the
# call's answer to the version that runs later: the arguments run in order.
class S
  def self.setup
    def self.v = :body
  end
end
begin; p S.v; rescue NoMethodError => e; p e.class; end
p S.setup, (def S.v = :placed), S.v
p S.setup, S.v
p((def S.v = :first), S.setup, S.v)
# A def after an argument or a receiver that leaves the body early does not run with the body.
class J
  def self.setup
    p([1].each { return :stopped }, (def J.made = :ok))
  end
end
begin; p J.made; rescue NoMethodError => e; p e.class; end
p J.setup
begin; p J.made; rescue NoMethodError => e; p e.class; end
