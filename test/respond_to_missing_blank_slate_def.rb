# A respond_to_missing? defined under a run-time condition that did not run, in
# a class under BasicObject with no Kernel: the name is the def's own, with
# nothing behind it (NoMethodError), as for any other def.
class Off < BasicObject
  if ::ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
  def t = respond_to_missing?(:x, false)
  def u = respond_to_missing?(:x)
end

class Sub < Off
  def s = respond_to_missing?(:x, true)
end

def try
  ::Kernel.p yield
rescue ::NoMethodError
  ::Kernel.p :nme
end

try { Off.new.t }
try { Off.new.u }
try { Sub.new.s }
