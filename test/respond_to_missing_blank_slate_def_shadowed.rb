# A conditional respond_to_missing? whose class only looks like a blank
# slate by its names: `class P::Q < BasicObject` is not the top-level Q, and
# a bare BasicObject inside a module that declares its own BasicObject is
# that class, an Object. Both keep Kernel's respond_to_missing?.
module P; end
class P::Q < BasicObject; end
class Q
  if ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
end
p Q.new.respond_to?(:zz)
p Q.new.send(:respond_to_missing?, :zz, false)

module R
  class BasicObject; end
  class Off < BasicObject
    if ARGV.size == 9
      def respond_to_missing?(n, p = false) = true
    end
  end
end
p R::Off.new.respond_to?(:zz)
p R::Off.new.send(:respond_to_missing?, :zz, false)
