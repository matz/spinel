# The implicit conversions of a split call's operands, run ahead of the call as CRuby runs them: a block operand's to_proc (a BasicObject's, a private one, one whose class defines is_a? and nil?, one answering no Proc) and a ** operand's to_hash (a BasicObject's, one answering no Hash, none at all).
class BO < BasicObject
  def to_proc = ::Kernel.proc { |a| a + 1 }
end
class Od
  def is_a?(k) = true
  def nil? = true
  def to_proc = proc { |a| a * 7 }
end
class Pt
  private def to_proc = proc { |a| a * 5 }
end
class Bad; def to_proc = 42; end
e = {}
bo = BO.new
od = Od.new
pt = Pt.new
p [1, 2].map(**e, &bo)
p [1, 2].map(**e, &od)
p [1, 2].map(**e, &pt)
b = Bad.new
begin; [1].map(**e, &b); rescue TypeError => x; p x.message; end
@b = b
begin; [1].map(**e, &@b); rescue TypeError => x; p x.message; end
class BH < BasicObject
  def to_hash = {}
end
class Bh; def to_hash = 1; end
pr = proc { |a| a + 1 }
bh0 = BH.new
p [1].push(2, **bh0, &pr)
bh = Bh.new
begin; [1].push(2, **bh, &pr); rescue TypeError => x; p x.message; end
o = Object.new
begin; [1].push(2, **o, &pr); rescue TypeError => x; p x.message; end
