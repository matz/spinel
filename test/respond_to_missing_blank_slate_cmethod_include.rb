# A respond_to_missing? defined under a run-time condition in a class under
# BasicObject whose class method includes Kernel, called from outside:
# CRuby's call then finds Kernel's method behind the def (false), which the
# class's Kernel cell, set by the include as it runs, says.
class X < BasicObject
  def self.mix = include(::Kernel)
  if ::ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
  def t = respond_to_missing?(:x, false)
end
X.mix
::Kernel.p X.new.t
