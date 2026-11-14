# The superclass of the class that has the conditional respond_to_missing? is
# BasicObject written with a leading `::`.
class X < ::BasicObject
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

try { X.new.t }
