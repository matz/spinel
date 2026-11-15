# The def under a condition that did not run sits in a reopen that names no
# superclass, after the class line that names BasicObject.
class Re < BasicObject; end
class Re
  if ::ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
  def t = respond_to_missing?(:x, false)
end

def try
  ::Kernel.p yield
rescue ::NoMethodError
  ::Kernel.p :nme
end

try { Re.new.t }
