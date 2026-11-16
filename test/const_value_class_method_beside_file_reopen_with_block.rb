# A class method with a block parameter on a reopened builtin (File) beside a constant naming one of several classes: the class that is not the constant's is still marked, whichever way the call is lowered, so the answers are the version at each point.
class File
  def self.hi(&b) = b ? b.call(9) : 0
end
class A; def self.hi = 1; end
class B; end
class Other; def self.hi = 5; end
S = [1, 2].sum == 3 ? A : B
p S.hi
class Other; def self.hi = 6; end
p Other.hi
