# A blank slate whose Kernel a cell decides answers a Kernel-row call in a
# method it holds by the class of the receiver, as CRuby looks the method up
# from there: a subclass reads its own cell (a class method's include run on
# it), has Kernel for sure (an include in its body), or reads the cell of a
# module it includes (a class method's include, or a conditional one, in the
# module). The holder's own instances keep the holder's answer.
def show
  ::Kernel.p yield
rescue ::NoMethodError
  ::Kernel.p :nme
end

# a class method's include run on a subclass, a deeper one, and one called by super
class Mix < BasicObject
  def self.mix = include(::Kernel)
  def t = respond_to_missing?(:x, false)
end
class MixSub < Mix; end
class MixDeep < MixSub; end
class MixSuper < Mix
  def t = [:sup, super]
end
MixDeep.mix
show { MixDeep.new.t }
show { MixSub.new.t }
show { Mix.new.t }
MixSuper.mix
show { MixSuper.new.t }

# the subclass is never made, or overrides the method: the holder's answer
class Idle < BasicObject
  def self.mix = include(::Kernel)
  def t = respond_to_missing?(:x, false)
end
class IdleSub < Idle
  def t = :own
end
IdleSub.mix
show { Idle.new.t }
show { IdleSub.new.t }
Idle.mix
show { Idle.new.t }

# the holder's Kernel include is conditional; a subclass has Kernel for sure,
# or through a module whose class method, or whose own conditional include, brings it
module ViaMix
  def self.mix = include(::Kernel)
end
module ViaCond
  include ::Kernel if ::ARGV.size == 0
end
class Cond < BasicObject
  include ::Kernel if ::ARGV.size == 3
  def t = respond_to_missing?(:x, false)
  def u = [1].map { respond_to_missing?(:x, false) }
end
class CondKernel < Cond
  include ::Kernel
end
class CondViaMix < Cond
  include ::ViaMix
end
class CondViaCond < Cond
  include ::ViaCond
end
class CondSite < Cond
  def self.mix = include(::Kernel)
end
show { CondKernel.new.t }
show { CondViaMix.new.t }
ViaMix.mix
show { CondViaMix.new.t }
show { CondViaCond.new.u }
show { CondSite.new.t }
CondSite.mix
show { CondSite.new.t }
show { Cond.new.t }

# a namespaced subclass, beside a top-level class of the same leaf name
class Spaced < BasicObject
  def self.mix = include(::Kernel)
  def t = respond_to_missing?(:x, false)
end
class Leaf < Spaced; end
module NS
  class Leaf < ::Spaced; end
end
NS::Leaf.mix
show { NS::Leaf.new.t }
show { Leaf.new.t }

# an instance with a singleton method; a holder's method_missing answers where no cell is set
class Single < BasicObject
  def method_missing(n, *a) = [:mm, n]
  def self.mix = include(::Kernel)
  def t = respond_to_missing?(:x, false)
end
class SingleSub < Single; end
s = SingleSub.new
def s.extra = 1
show { s.t }
SingleSub.mix
show { s.t }
show { Single.new.t }

# super that reaches no method, where the subclass has Kernel for sure
class Sup < BasicObject
  include ::Kernel if ::ARGV.size == 3
  def to_s = super
end
class SupSub < Sup
  include ::Kernel
end
show { SupSub.new.to_s.start_with?("#<SupSub") }
show { Sup.new.to_s }
