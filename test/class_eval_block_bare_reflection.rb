# A bare reflective query in a class_eval block asks the class, as self there
class Foo
  def self.greet = "hi"
  def inst = 1
  @count = 3
end

Foo.class_eval { p respond_to?(:greet) }
Foo.class_eval { p respond_to?(:inst) }
Foo.class_eval { p respond_to?(:nothing) }
Foo.class_eval { p method_defined?(:inst) }
Foo.class_eval { p method_defined?(:greet) }
Foo.class_eval { p instance_variable_get(:@count) }
Foo.class_eval { p send(:greet) }
Foo.class_eval { p name }
Foo.class_eval { p is_a?(Class) }

class Foo
  attr_reader :v
  def initialize = @v = 5
end
Foo.class_eval { p new.v }
Foo.class_eval { p superclass }
Foo.class_eval { p ancestors.first }

def hook(views)
  Foo.class_eval { views << greet if respond_to?(:greet) }
  views
end
p hook([])
