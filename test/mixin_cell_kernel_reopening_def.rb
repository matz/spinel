# A blank slate whose Kernel a cell decides (an include under a condition
# that does not run) answers a guarded Kernel-row call in a method it holds
# by a subclass's own method of the name, or its own method_missing, where a
# reopening of the subclass makes it before any object of the subclass
# exists: a forward-declared class. A reopening that runs after an object
# was made counts from where it runs (the holder's answer before it). The
# same holds for such a method of a module the subclass includes or
# prepends, and for one a def under a condition makes, where it ran.
def show
  ::Kernel.p yield
rescue ::NoMethodError
  ::Kernel.p :nme
end

class Holder < BasicObject
  include ::Kernel if ::ARGV.size == 3
  def t = respond_to_missing?(:x, false)
end

# forward-declared, reopened before any object is made
class FwdOwn < Holder; end
class FwdOwn
  def respond_to_missing?(n, p) = :fwd_own
end
class FwdMiss < Holder; end
class FwdMiss
  def method_missing(n, *a) = [:fwd_miss, n]
end
class FwdDeep < FwdOwn; end

# reopened after an object was made
class LateOwn < Holder; end
early = LateOwn.new
show { early.t }
class LateOwn
  def respond_to_missing?(n, p) = :late_own
end

# a method of the name from a module the subclass includes or prepends, one of
# a module included under a condition that does not run, and a def under a
# condition in the subclass's body, run or not
module ModOwn
  def respond_to_missing?(n, p) = :mod_own
end
module ModMiss
  def method_missing(n, *a) = [:mod_miss, n]
end
class ViaInclude < Holder
  include ::ModOwn
end
class ViaPrepend < Holder
  prepend ::ModOwn
end
class ViaMissing < Holder
  include ::ModMiss
end
class ViaCondInclude < Holder
  include ::ModOwn if ::ARGV.size == 4
end
class CondRan < Holder
  if ::ARGV.size == 0
    def respond_to_missing?(n, p) = :cond_ran
  end
end
class CondNot < Holder
  if ::ARGV.size == 5
    def respond_to_missing?(n, p) = :cond_not
  end
end

show { FwdOwn.new.t }
show { FwdDeep.new.t }
show { FwdMiss.new.t }
show { Holder.new.t }
show { ViaInclude.new.t }
show { ViaPrepend.new.t }
show { ViaMissing.new.t }
show { ViaCondInclude.new.t }
show { CondRan.new.t }
show { CondNot.new.t }
