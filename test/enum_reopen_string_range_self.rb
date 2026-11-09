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
