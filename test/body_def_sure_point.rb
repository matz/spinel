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
