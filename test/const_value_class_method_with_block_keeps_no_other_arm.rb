# A class method called with a block on a constant that may name several classes is lowered by the proc-form dispatch, not the arm switch: a class that is not the constant's (Other, reopened later) gets no arm, and no version of it is kept.
class A; def self.hi(&b) = b ? b.call("A1") : "A1n"; end
class B; end
class Other; def self.hi(&b) = b ? b.call("O1") : "O1n"; end
S = [1, 2].sum == 3 ? A : B
p S.hi { |x| x * 2 }
class Other; def self.hi(&b) = "O2"; end
p Other.hi
