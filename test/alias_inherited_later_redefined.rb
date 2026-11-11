# An alias of an inherited method keeps the ancestor's body in effect where
# it appears: a later redefinition in the ancestor does not take its place.
class P
  def m = 1
end
class D < P
  alias o m
end
class P
  def m = 2
end
p D.new.o, D.new.m

# a define_method original, with the local its block closes over
class E
  count = 5
  define_method(:m) { |y = 1| count + y }
end
class F < E
  alias_method :n, :m
end
class E
  def m = 2
end
p F.new.n, F.new.n(10), F.new.m
