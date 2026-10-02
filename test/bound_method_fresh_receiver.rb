# GC: a fresh receiver of <recv>.method(:sym) is referenced only by the Method
# being built. The constructor allocates, so an unrooted receiver is swept
# before it is stored as `self` -- the GC verifier reports a non-heap/corrupt
# object and a later call reads a dangling self. The bind site must hold the
# receiver in a rooted C temporary across the constructor. Run under GC stress
# by gc-minor-test. The same hazard applies to UnboundMethod#bind,
# Method#super_method, and the generic Method#to_proc fallback.

class FreshRecv
  def initialize(n) = @n = n
  def v = @n
  def plus(x) = @n + x
end

class FreshParent
  def v = 100
end

class FreshChild < FreshParent
  def v = 200
end

# object receiver, bound and called immediately
puts FreshRecv.new(7).method(:v).call

# the Method survives through a poly slot; the receiver only through the Method
a = [FreshRecv.new(41).method(:plus)]
puts a[0].call(1)

# UnboundMethod#bind holds a fresh object
puts FreshRecv.instance_method(:v).bind(FreshRecv.new(13)).call

# Method#super_method: the source Method is a fresh temporary
puts FreshChild.new.method(:v).super_method.call

# A fresh Method returned into a poly slot (`pick_plus` widens to Method|Integer)
# reaches the poly-receiver #to_proc / #unbind / #parameters arms, each of which
# allocates while it reads the Method out of the sp_RbVal temp. The temp must be
# rooted across that allocation or the collector sweeps it and the constructor
# stores a dangling pointer (the verifier reports a corrupt object).
def pick_plus(c) = c ? FreshRecv.new(41).method(:plus) : 0
held = []
300.times do
  held << pick_plus(true).to_proc
  held << pick_plus(true).unbind
  held << pick_plus(true).parameters
end
puts held.length
puts pick_plus(true).unbind.name
puts pick_plus(true).parameters.length

# The generic Method#to_proc fallback allocates the proc while the receiver
# Method is otherwise unreachable (target unresolved: self.class names no one
# class, as FreshClassValue has a subclass). Storing the proc forces
# the allocation to happen with GC stress on.
class FreshClassValue
  def self.cm(a) = a
  def go = self.class.method(:cm).to_proc
end
class FreshClassSub < FreshClassValue; end
kept = []
300.times { kept << [FreshClassValue.new.go] }
puts "ok"
