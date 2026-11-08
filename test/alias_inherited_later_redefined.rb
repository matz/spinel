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
