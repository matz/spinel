# The conditional respond_to_missing? is in BasicObject's own body: a blank
# slate below it has no Kernel to answer where the branch did not run.
class BasicObject
  if ::ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
end
class X < BasicObject
  def t = respond_to_missing?(:x, false)
end

def try
  ::Kernel.p yield
rescue ::NoMethodError
  ::Kernel.p :nme
end

try { X.new.t }
