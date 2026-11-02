# spinel: rbs-seed-run
# --rbs signatures find their method by name, class and kind. Same-named
# methods of unrelated classes, and an instance method beside a class method
# of its name, each take their own signature; a signature declared twice
# identically (the sig file repeats SmlTwin#twice) is the one declaration;
# the first of two definitions of a method in one class is the one a
# signature reaches (the second is the one that runs).
class SmlA
  def v(x) = x + 1
  def self.v(x) = x.to_s
end

class SmlB
  def v(x) = x * 2
  def self.v(x) = x.upcase
end

class SmlTwin
  def twice(x) = x * 2
end

class SmlTwice
  def m(x) = x + 1
  def m(x) = x.to_s
end

puts SmlA.new.v(1)
puts SmlA.v(1)
puts SmlB.new.v("a").inspect rescue puts "err"
puts SmlB.v("b")
puts SmlTwin.new.twice(4)
puts SmlTwice.new.m(1).inspect
