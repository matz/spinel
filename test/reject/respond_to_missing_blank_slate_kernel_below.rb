# A respond_to_missing? defined under a run-time condition in a class under
# BasicObject, called on a subclass that includes Kernel: CRuby's lookup
# meets Kernel's respond_to_missing? in front of the def, whether or not its
# branch ran, where the def's own class raises NoMethodError. The version
# entry stands for the def's class alone, so the call is refused.
class Off < BasicObject
  if ::ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
end
class Sub < Off
  include ::Kernel
  def s = respond_to_missing?(:x, true)
end
p Sub.new.s
