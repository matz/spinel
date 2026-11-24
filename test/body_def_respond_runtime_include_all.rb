# A respond_to? of a name the run computes, whose include_all argument the run decides too,
# answers for a private def a method body makes by the version cell, for both answers: the public
# one (false until the body ran, and after it for a private def) and every visibility's.
class K
  def self.setup = (private def m = :k)
  def self.setup2 = (protected def pm = 1)
  def n = 1
end
nm = [:m, :n][ARGV.size]
fl = ARGV.size == 0
k = K.new
p [k.respond_to?(nm, fl), k.respond_to?(nm, !fl), k.respond_to?(nm)]
K.setup
p [k.respond_to?(nm, fl), k.respond_to?(nm, !fl), k.respond_to?(nm)]
p [k.respond_to?(nm, true), k.respond_to?(nm, false), k.respond_to?(nm, nil), k.respond_to?(nm, fl && nil)]

# a boxed receiver, and a name that is protected
class J; def m = 2; end
xs = [K.new, J.new, "s", 3]
p xs.map { |x| x.respond_to?(nm, fl) }
p xs.map { |x| x.respond_to?(nm, !fl) }
pn = [:pm, :n][ARGV.size]
p [k.respond_to?(pn, fl), k.respond_to?(pn, !fl)]
K.setup2
p [k.respond_to?(pn, fl), k.respond_to?(pn, !fl)]

# a second class, with an unrelated one beside it
class A
  def self.setup = (private def am = :a)
end
an = [:am, :n][ARGV.size]
ys = [A.new, K.new]
p ys.map { |x| [x.respond_to?(an, fl), x.respond_to?(an, !fl)] }
A.setup
p ys.map { |x| [x.respond_to?(an, fl), x.respond_to?(an, !fl)] }
