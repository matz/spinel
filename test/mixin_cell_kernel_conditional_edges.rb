# A mixin that may not run brings Kernel only where it ran, and a module's Kernel
# include that is conditional is no certain Kernel for the class that includes it:
# the include of such a module in a class method, or under a condition, does not give
# the blank slate Kernel where the module's own include did not run. A call whose
# operands run code evaluates them before it finds no method, and a blank slate's own
# method_missing answers a Kernel-row name where no cell is set.
module CondKernel; include ::Kernel if ::ARGV.size == 3; end
class ViaMethod < BasicObject
  def self.setup = include(::CondKernel)
  setup
  def to_s = super
end
class ViaBody < BasicObject
  include ::CondKernel if ::ARGV.size == 0
  def to_s = super
end

module Holder; def self.mix = include(::Kernel); end
Holder.mix
class ViaIdleEdge < BasicObject
  include ::Holder if ::ARGV.size == 3
  def to_s = super
end

$count = 0
class Operands < BasicObject
  def self.mix = include(::Kernel)
  mix if ::ARGV.size == 3
  def t = respond_to_missing?(($count += 1), false)
end

class Answers < BasicObject
  def method_missing(n, *a) = [:mm, n]
  def self.mix = include(::Kernel)
  mix if ::ARGV.size == 3
  def t = respond_to_missing?(:x, false)
end

class Blocky < BasicObject
  def method_missing(n, *a, &b) = b ? [:blk, b.call] : [:noblk]
  def self.mix = include(::Kernel)
  mix if ::ARGV.size == 3
  def t = respond_to_missing?(:x, false) { 42 }
end

def attempt
  p yield
rescue NoMethodError
  puts "NoMethodError"
end

attempt { ViaMethod.new.to_s.start_with?("#<ViaMethod") }
attempt { ViaBody.new.to_s.start_with?("#<ViaBody") }
attempt { ViaIdleEdge.new.to_s.start_with?("#<ViaIdleEdge") }
attempt { Operands.new.t }
p $count
attempt { Answers.new.t }
attempt { Blocky.new.t }
