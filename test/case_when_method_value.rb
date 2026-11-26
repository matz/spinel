# A Method value as a `when` pattern is called with the subject, as
# Method#=== does, in the statement and the value forms of `case`. It was
# compared with the subject as a pointer. A nil Method is nil's pattern.
class C
  def big(x) = x > 3
  def hi(x) = x * 3
end
class D < C
  def hi(x) = super + 1
end
live = C.new.method(:big)
[2, 4].each do |n|
  case n
  when live then p [:big, n]
  else p [:small, n]
  end
end
r = case 4 when live then :v_big else :v_small end
p r
sup = D.new.method(:hi).super_method
case 1
when sup then p :sup_hit
end
m = C.new.method(:hi).super_method
case 5
when m then p :hit
else p :miss
end
case nil
when m then p :nil_hit
else p :nil_miss
end
# a Method reached only through `when` takes the subject's type, and the
# subject stays rooted while the Method's body allocates
class E
  def churn(x)
    a = []
    20_000.times { |i| a << "s#{i}" }
    x.size > 100
  end
end
churn = E.new.method(:churn)
s = "subject-" + "x" * 40
r2 = case s
     when churn then :churn
     when "subject-" + "x" * 40 then :eq
     else :none
     end
p r2
# a builtin's bound Method is called the same way
incl = [1, 2].method(:include?)
case 2
when incl then p :incl
else p :no
end
sw = "abc".method(:start_with?)
p(case "ab" when sw then :sw else :no end)
lt = 5.method(:<)
case 7 when lt then p :gt else p :le end
# a builtin's Method read out of an Array, as a `when` pattern and with
# `===` or call (item 274)
[[1, 2].method(:include?)].each { |pat| case 2; when pat then p :ehit; else p :emiss; end }
pats = [[1, 2].method(:include?)]
case 2
when pats[0] then p :ahit
else p :amiss
end
[C.new.method(:big), [1, 2].method(:include?)].each { |pat| case 2; when pat then p :mhit; else p :mmiss; end }
p(pats[0] === 2, pats[0].call(3))
[[1, 2].method(:include?)].each { |pat| p(pat === 1) }
swp = ["abc".method(:start_with?)]
p(case "ab" when swp[0] then :swhit else :swmiss end)
# an Integer subject that can be nil
incz = [0].method(:include?)
def pick(f) = f ? 0 : nil
[false, true].each { |f| w = pick(f); p(case w when incz then :inc else :none end) }
# a mixed Array of Methods against a String subject made for the case:
# the boxed pattern is called with it while churn allocates, and a later
# arm reads it
[E.new.method(:churn), ["zz"].method(:include?)].each do |pat|
  p(case "subject-" + "x" * 40
    when pat then :pat
    when "subject-" + "x" * 40 then :beq
    else :none
    end)
end
# a Method that reaches the Array through a call, a method's result or an
# object's reader is called by the boxed `when` with the String subject
class F
  def churn2(s) = s.size > 3
end
def stash(list, m) = list << m
l1 = []
stash(l1, F.new.method(:churn2))
l1.each { |pat| r = case "s#{pat.name}" when pat then :hit else :eq end; p [pat.name, r] }
def make_m = F.new.method(:churn2)
l2 = [make_m]
l2.each { |pat| r = case "s#{pat.name}" when pat then :hit else :eq end; p [pat.name, r] }
class Holder
  def initialize(m) = @m = m
  def m = @m
end
l3 = [Holder.new(F.new.method(:churn2)).m]
l3.each { |pat| r = case "s#{pat.name}" when pat then :hit else :eq end; p [pat.name, r] }
# a capture whose write is an operand (an Array element) is boxed too
def check2(x) = x * 2
p check2(0)
patterns = [(cm = method(:check2))]
p(case "ab" when patterns[0] then :chit else :cmiss end)
