# A call on a builtin value of a method a module included into Object
# defines beside a top-level def of its name, made after the call in a loop:
# the version entry the call runs is a function of its own, which the
# reachability pass reaches for a receiver no class of the program holds.
module M
  def km = :mod
end
include M
r = []
2.times do |i|
  r << 1.send(:km)
  r << "s".send(:km)
  def km = :top if i == 0
end
p r
module N
  def nm(x) = [:n, x]
end
include N
p 1.nm(2), nil.nm(3)
def nm(x) = [:top, x]
p 1.send(:nm, 4), "s".send(:nm, 5)
