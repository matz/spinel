# A class built with `X = Class.new` and reopened with `class X` in the
# same module stays one class when another module has a class of that name.
module Outer
  Bar = Class.new do
    def b = 1
  end
  class Bar
    def k = 2
  end
end
module Other
  Bar = Class.new { def b = 7 }
  class Bar; def k = 8; end
end
module Third
  class Bar; def j = 9; end
end
p Outer::Bar.new.k
p Outer::Bar.new.b
p Other::Bar.new.k
p Other::Bar.new.b
p Third::Bar.new.j
p Outer::Bar.equal?(Other::Bar)
p Outer::Bar.name, Other::Bar.name, Third::Bar.name
p Outer::Bar.new.respond_to?(:j)

# A module built with Module.new and reopened with `module`
module Outer
  Mix = Module.new { def m1 = :m1 }
  module Mix; def m2 = :m2; end
end
module Other
  module Mix; def m2 = :other; end
end
class UsesMix; include Outer::Mix; end
p UsesMix.new.m1, UsesMix.new.m2

# The builder in a nested module, with a superclass, reopened in a
# second body of the module
module A
  module B
    Err = Class.new(StandardError) { def b = 1 }
  end
end
module A
  module B
    class Err; def k = 2; end
  end
end
module C
  Err = Class.new { def b = 7 }
end
p A::B::Err.new.k, A::B::Err.ancestors.include?(StandardError), C::Err.new.b, A::B::Err.name
