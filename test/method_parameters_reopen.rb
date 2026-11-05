# A builtin class's reopen that defines a builtin's name: a Method made
# where the reopen's def is live calls the program's def, and answers that
# def's #arity and #parameters; one made ahead of it answers the builtin's

mb = "s".method(:center)
p [mb.arity, mb.parameters]

class String
  def center(w, pad = " ", extra: 0) = "user"
end
class Array
  def first(a, b) = a
end
cm = "s".method(:center)
p [cm.call(5), cm.arity, cm.parameters]
p [mb.arity, mb.parameters]
fm = [1].method(:first)
p [fm.call(3, 4), fm.arity, fm.parameters]
ms = [cm, fm]
p ms.map { |m| [m.arity, m.parameters] }
def params_of(m) = m.parameters
p params_of(cm)

# passed as a block
m = "ab".method(:center)
p ["ab"].map(&m)
p m.parameters

# a reopen of a module the builtin class includes
module Comparable
  def clamp(lo, hi = nil, strict: false) = :mine
end
p [3.method(:clamp).arity, 3.method(:clamp).parameters]

# a reopen of Object: a true's or false's own method (TrueClass's &,
# FalseClass's to_s) stays the builtin's; a Method of one they get from
# Kernel is the reopen's
class Object
  def &(o, z = 1) = :obj
  def to_s(q = 1) = "objts"
  def frozen?(x = 0) = :objfrozen
end
p [true.method(:&).arity, false.method(:&).arity, true.method(:to_s).arity]
p [true.method(:frozen?).parameters, true.method(:frozen?).arity, false.method(:frozen?).arity]
