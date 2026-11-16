# A blank slate whose Kernel a cell decides answers a Kernel-row call in a
# method it holds by the class of the receiver, as CRuby looks the method up
# from there: a subclass reads its own cell (a class method's include run on
# it), has Kernel for sure (an include in its body), or reads the cell of a
# module it includes (a class method's include, or a conditional one, in the
# module). The holder's own instances keep the holder's answer. Where none
# is set, a subclass's own method_missing answers, and the NoMethodError
# names the receiver's class, as a version entry's does; an include in a
# reopening of the class, after a call ran, holds only from there on.
def show
  ::Kernel.p yield
rescue ::NoMethodError
  ::Kernel.p :nme
end

# The classes whose own methods answer stand ahead of the first call that may
# make an object of any class (a class method's include run on a class, an
# object's singleton method): a def after one counts as one a call may run
# ahead of, and keeps the holder's answer.

# a subclass's own method_missing answers where no cell is set
class MissPlain < BasicObject
  include ::Kernel if ::ARGV.size == 3
  def t = respond_to_missing?(:x, false)
  def u = respond_to_missing?(:y, true)
end
class MissPlainSub < MissPlain
  def method_missing(n, *a) = [:plain_sub, n]
end

# a subclass that defines the name itself: its own method answers, whatever Kernel says
class OwnName < BasicObject
  def self.mix = include(::Kernel)
  def t = respond_to_missing?(:x, false)
end
class OwnNameSub < OwnName
  def respond_to_missing?(n, p) = :sub_own
end
class OwnNameDeep < OwnNameSub; end
class OwnNameMiss < OwnName
  def method_missing(n, *a) = [:miss, n]
  def respond_to_missing?(n, p) = :miss_own
end

# an undef of the name below the subclass ends the lookup there
class OwnUndef < BasicObject
  include ::Kernel if ::ARGV.size == 3
  def t = respond_to_missing?(:x, false)
end
class OwnUndefSub < OwnUndef
  def respond_to_missing?(n, p) = :sub_own
end
class OwnUndefLeaf < OwnUndefSub
  undef_method :respond_to_missing?
end

# a subclass's method_missing undefined in its own body, or in a class below it
class UndefMiss < BasicObject
  include ::Kernel if ::ARGV.size == 3
  def t = respond_to_missing?(:x, false)
end
class UndefMissOwn < UndefMiss
  def method_missing(n, *a) = [:undef_own, n]
  undef_method :method_missing
end
class UndefMissSub < UndefMiss
  def method_missing(n, *a) = [:undef_sub, n]
end
class UndefMissLeaf < UndefMissSub
  undef_method :method_missing
end

# an undef in a module ends the lookup where the module stands: prepended to
# the subclass, included in a class below it, or undefining method_missing
module ModUndefOwn
  def respond_to_missing?(n, p) = :mod_own
  undef_method :respond_to_missing?
end
module ModUndefMiss
  def method_missing(n, *a) = [:mod_mm, n]
  undef_method :method_missing
end
class ModUndef < BasicObject
  include ::Kernel if ::ARGV.size == 3
  def t = respond_to_missing?(:x, false)
end
class ModUndefPre < ModUndef
  prepend ::ModUndefOwn
  def respond_to_missing?(n, p) = :pre_own
end
class ModUndefSub < ModUndef
  def respond_to_missing?(n, p) = :sub_own
  def method_missing(n, *a) = [:sub_mm, n]
end
class ModUndefLeaf < ModUndefSub
  include ::ModUndefOwn
end
class ModUndefMissLeaf < ModUndefSub
  undef_method :respond_to_missing?
  include ::ModUndefMiss
end

# an alias in the subclass's body answers with its original; a module's own
# include brings its module's method
module NestInner
  def respond_to_missing?(n, p) = :nest_inner
end
module NestOuter
  include ::NestInner
end
class Aliased < BasicObject
  include ::Kernel if ::ARGV.size == 3
  def t = respond_to_missing?(:x, false)
end
class AliasedSub < Aliased
  def own(n, p) = :alias_own
  alias respond_to_missing? own
end
class Nested < Aliased
  include ::NestOuter
end

# the call's operands, run once, and its block reach a subclass's method_missing
class BlockGiven < BasicObject
  include ::Kernel if ::ARGV.size == 3
  def c = respond_to_missing?(:c, false) { |x| x * 5 }
  def f(&blk) = respond_to_missing?(:f, true, &blk)
  def g(log) = respond_to_missing?(log.push(:g).size, false)
end
class BlockGivenSub < BlockGiven
  def method_missing(n, *a, &b) = [:block_sub, n, a, (b ? yield(2) : :noblk)]
end
class BlockGivenAlias < BlockGiven
  def miss(n, *a) = [:alias_mm, n, a]
  alias method_missing miss
end
class BlockGivenAliasMethod < BlockGiven
  def miss(n, *a) = [:alias_method_mm, n, a]
  alias_method :method_missing, :miss
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

# a subclass's own method_missing answers where no cell is set, the holder's elsewhere
class Miss < BasicObject
  def self.mix = include(::Kernel)
  def method_missing(n, *a) = [:holder, n]
  def t = respond_to_missing?(:x, false)
end
class MissSub < Miss
  def method_missing(n, *a, &b) = [:sub, n, a.size, b ? b.call : nil]
end
show { MissSub.new.t }
show { Miss.new.t }
show { MissPlainSub.new.u }
MissSub.mix
show { MissSub.new.t }
show { Miss.new.t }

# the NoMethodError names the receiver's class
class Named < BasicObject
  include ::Kernel if ::ARGV.size == 3
  def t = respond_to_missing?(:x, false)
end
class NamedSub < Named; end
begin; NamedSub.new.t; rescue ::NoMethodError => e; ::Kernel.p [e.message, e.name]; end
begin; Named.new.t; rescue ::NoMethodError => e; ::Kernel.p e.message; end
class NamedDef < BasicObject
  if ::ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
  def t = respond_to_missing?(:x, false)
end
class NamedDefSub < NamedDef; end
begin; NamedDefSub.new.t; rescue ::NoMethodError => e; ::Kernel.p e.message; end

# an include in a reopening, after a call ran: Kernel from there on
class Late < BasicObject
  def t = respond_to_missing?(:x, false)
end
class LateSub < Late; end
show { Late.new.t }
class Late; include ::Kernel; end
show { Late.new.t }
class LateCond < BasicObject
  include ::Kernel if ::ARGV.size == 3
  def t = respond_to_missing?(:x, false)
end
class LateCondSub < LateCond; end
show { LateCondSub.new.t }
class LateCondSub; include ::Kernel; end
show { LateCondSub.new.t }
show { LateCond.new.t }

# a subclass that defines the name itself, and an undef below it
show { OwnNameSub.new.t }
show { OwnNameDeep.new.t }
show { OwnNameMiss.new.t }
show { OwnName.new.t }
show { OwnUndefSub.new.t }
show { OwnUndefLeaf.new.t }

# a subclass's method_missing made in a reopening is not there before it runs
class LateMiss < BasicObject
  include ::Kernel if ::ARGV.size == 3
  def t = respond_to_missing?(:x, false)
end
class LateMissSub < LateMiss; end
show { LateMissSub.new.t }
class LateMissSub
  def method_missing(n, *a) = [:late, n]
end

# a reopening that runs before any object is made: its include holds from the start
class Ahead < BasicObject; end
class Ahead
  include ::Kernel
  def t = respond_to_missing?(:x, false)
end
show { Ahead.new.t }

# an object made by a send ahead of the reopening: the include holds from where it runs
class SentNew < BasicObject
  def t = respond_to_missing?(:x, false)
end
sent = SentNew.send(:new)
psent = SentNew.public_send(:new)
show { sent.t }
class SentNew
  include ::Kernel
end
show { sent.t }
show { psent.t }

# an instance with a singleton method answers as the other instances of its
# class do, below the holder and the holder's own, with or without the
# holder's method_missing
class SgHold < BasicObject
  include ::Kernel if ::ARGV.size == 3
  def t = respond_to_missing?(:x, false)
end
class SgSub < SgHold; end
class SgMiss < BasicObject
  include ::Kernel if ::ARGV.size == 3
  def method_missing(n, *a) = [:sg_mm, n]
  def t = respond_to_missing?(:x, false)
end
sg_plain = SgSub.new
sg_sub = SgSub.new
def sg_sub.x = 1
sg_hold = SgHold.new
def sg_hold.x = 1
sg_miss = SgMiss.new
def sg_miss.x = 1
show { sg_plain.t }
show { sg_sub.t }
show { sg_hold.t }
show { SgHold.new.t }
show { sg_miss.t }
show { SgMiss.new.t }

# a method_missing undefined in a reopening of the subclass, after an object was made
class UndefLater < UndefMiss
  def method_missing(n, *a) = [:undef_later, n]
end
undef_later = UndefLater.new
class UndefLater
  undef_method :method_missing
end
show { UndefMissOwn.new.t }
show { UndefMissLeaf.new.t }
show { UndefMissSub.new.t }
show { undef_later.t }

# the module undefs, aliases and nested include
show { ModUndefPre.new.t }
show { ModUndefLeaf.new.t }
show { ModUndefSub.new.t }
show { ModUndefMissLeaf.new.t }
show { AliasedSub.new.t }
show { Nested.new.t }

# the operands and the block
show { BlockGivenSub.new.c }
show { BlockGivenSub.new.f { |v| v * 10 } }
show { BlockGivenSub.new.f }
given_log = []
show { BlockGivenSub.new.g(given_log) }
show { BlockGiven.new.g(given_log) }
::Kernel.p given_log
show { BlockGivenAlias.new.g(given_log) }
show { BlockGivenAliasMethod.new.g(given_log) }

# an alias made in a reopening, after an object was made, is not there before it runs
class AliasLate < Aliased
  def own(n, p) = :alias_late
end
alias_late = AliasLate.new
show { alias_late.t }
class AliasLate
  alias respond_to_missing? own
end
