# spinel: gc-stress
# A method reopening Object, Numeric, Array or Hash takes self boxed, and a
# boxed receiver is often a new object only the call's argument holds: $~
# hands a new MatchData on every read. A String reopening's receiver is as
# often a new String (an interpolation). The method rooted its parameters
# but not self, so under SPINEL_GC_STRESS=2 the body's first allocation
# freed it.
class Object
  def pair(x) = [self, x]
  def tag = "<#{inspect}>"
end
class Array
  def twice = [self, self.size]
end
class Hash
  def keyed(k) = {k => self}
end
class String
  def two = [[1], self + "!"]
  def wrap(n) = "#{"<" * n}#{self}#{">" * n}"
end
"abc" =~ /(b)/
p $~.pair(1), $~.tag
m = "xyz".match(/y/)
p m.pair(:k)
"q1" =~ /\d/
p Regexp.last_match.pair(2)
i = 3
p [1, "a#{i}"].twice, {a: "b#{i}"}.keyed(:z)
p "a#{i}".two, "b#{i}".wrap(2), "lit".two
