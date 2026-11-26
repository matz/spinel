# spinel: reject-unsupported: (CallNode `class_eval`)
# A Class.new whose superclass defines inherited keeps one class for every
# evaluation of its site: a class_eval block on the local holding it is
# refused, since the one class would answer for each evaluation (CRuby
# prints [:inh, true, :inh, true]).
$h = []
class HB
  def self.inherited(s) = ($h << :inh)
end

def mk = (k = Class.new(HB) { def v = 1 }; k.class_eval { $h << (self == k) }; k)
a = mk
b = mk
p $h, a.new.v, [a, 1].first.equal?(a)
