# A program's Enumerable method called on a String Range gets the Range as
# self, as an Integer Range does. Before, Spinel passed the member Array, so
# is_a?(Range) was false and self.class was Array.
module Enumerable
  def to_set = [is_a?(Range) ? :range : :other, self]
  def kind = [self.class, first]
  def doubled
    r = []
    each { |x| r << x * 2 }
    r
  end
  def lens = map { |s| s.size }.sum
  def shout
    each { |x| yield x.upcase }
  end
  def drop_one(n) = select { |x| x != n }.size
end
r = ("a".."d")
p r.to_set
p ("a".."c").to_set
p r.kind
p (1..3).kind
p r.doubled
p ("aa".."ad").lens
("x".."z").shout { |u| print u }
puts
p r.drop_one("b")
p ("a"...).kind
def via_param(q) = q.to_set
p via_param("m".."n")
# Object's predicates on a String Range self (and on one in a local)
module Enumerable
  def preds = [nil?, equal?(self), is_a?(Enumerable), is_a?(Range)]
end
p r.preds
p [r.nil?, r.equal?(r), r.is_a?(Enumerable), (1.0..2.0).is_a?(Enumerable)]

# An endless String Range: Range#to_set raises before the program's to_set
def srange_to_set_of(x) = x.to_set
[-> { ("a"..).to_set }, -> { er = ("b"..); er.to_set }, -> { srange_to_set_of(("c"..)) }].each do |l|
  begin
    p l.call
  rescue RangeError => e
    p [e.class, e.message]
  end
end
p srange_to_set_of(("a".."b"))

# equal?, == and eql? with a boxed String Range, and the side effects
sr = ("a".."c")
box = [sr, 1, "x"]
p sr.equal?(box[0]), sr == box[0], sr.eql?(box[0]), sr == box[1], sr.equal?(box[2]), ("a".."d") == box[0]
def srange_eff(t) = (puts "eff #{t}"; t)
p srange_eff("a".."c").equal?(srange_eff(1))
p srange_eff("a".."c") == srange_eff("x")

# An ivar receiver gets the endless test too
class SrangeHolder
  def initialize(r) = @r = r
  def go = @r.to_set
end
begin
  p SrangeHolder.new(("a"..)).go
rescue RangeError => e
  p [e.class, e.message]
end
p SrangeHolder.new(("a".."b")).go
