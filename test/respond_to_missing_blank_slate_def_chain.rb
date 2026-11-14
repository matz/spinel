# The class that has the conditional respond_to_missing? reaches BasicObject
# through two program classes, one of them a reopen that writes no superclass.
class Root < BasicObject; end
class Mid < Root; end
class Mid; end
class Leaf < Mid
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

try { Leaf.new.t }
