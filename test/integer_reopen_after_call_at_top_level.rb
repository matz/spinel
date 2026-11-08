# A call on an Integer ahead of an Integer reopening's def of the method's
# name, at the top level, runs the builtin's method, as in CRuby; one after
# it runs the reopening's (#10719). A name builtins/integer.rb defines in
# Ruby (digits) is not covered: the reopening replaces that generic.
n = 3
p n.abs, n.succ, (-4).abs, 7.pred
class Integer
  def abs = :abs
  def succ = :succ
  def pred = :pred
end
p n.abs, n.succ, (-4).abs, 7.pred
def later(x) = [x.abs, x.succ]
p later(5)
