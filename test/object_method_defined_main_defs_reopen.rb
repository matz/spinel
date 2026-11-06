# Object's method_defined? family counts a top-level def beside the
# methods a reopened Object defines.
def foo(x) = x
class Object
  def bar = 1
end
p Object.private_method_defined?(:foo)
p Object.method_defined?(:bar)
p Object.method_defined?(:foo)
