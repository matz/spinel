# `unless Addr.method_defined?(:as_json, false)` around a reopening that
# defines as_json, where Addr's own body already defines it: the guard is
# decided at compile time, as the one-argument form is, and the def under it
# is dead, not one only the run time decides. With the second argument
# false, only the class's own bodies are asked, not its ancestors.
class Addr
  def as_json(*) = "own"
  def to_s = "addr"
end
unless Addr.method_defined?(:as_json, false)
  class Addr
    def as_json(options = nil) = "fallback"
  end
end
class Base
  def shared = 1
end
class Sub < Base; end
unless Sub.method_defined?(:shared, false)
  class Sub
    def shared = 2
  end
end
if Sub.method_defined?(:shared, true)
  class Sub
    def also = 3
  end
end
class Holder
  def self.held = @held
  def self.held=(v); @held = v; end
end
Holder.held = Addr.new
p Addr.new.as_json, Sub.new.shared, Sub.new.also
p Holder.held.public_methods(false).sort, Addr.instance_methods(false).sort
