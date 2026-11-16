# A class method called on a constant that may name one of several classes has an arm for every class that defines it, and each arm's version (here Other's, before it is reopened) is kept reachable.
class A
  def self.hi = "A1"
end
class B
end
class Other
  def self.hi = "Other1"
end
S = [1, 2].sum == 3 ? A : B
p S.hi
class Other
  def self.hi = "Other2"
end
unless $nope
end
class C1
end
class C2
end
T = case [1, 2].sum when 4 then C1 when 3 then C2 else Other end
