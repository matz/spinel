# An alias of a define_method whose name the program computes is bound by name,
# so the ancestor's later def of the name would take the alias's place.
class A
  [:m, :o].each { |nm| define_method(nm) { nm } }
end
class B < A
  alias_method :n, :m
end
class A
  def m = :new
end
p B.new.n
