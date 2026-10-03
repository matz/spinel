# obj.methods / public_methods / singleton_methods take an optional `all`
# argument. Only the bare form was compiled; with a literal `true` or `false`
# the call built and raised NoMethodError at run time. `true` is the bare
# form; `false` on an object with no singleton methods answers no singleton
# methods, and for public_methods the class's own public instance methods.
module M
  def m = 1
end

class P
  def pa = 1
  protected def pp_ = 1
end

class K < P
  include M
  def a = 1
  def b = 2
  protected def pr = 3
  private def c = 4
end

k = K.new
p k.methods(true) == k.methods
p k.public_methods(true) == k.public_methods
p k.singleton_methods(true)
p k.methods(false)
p k.public_methods(false).sort
p k.singleton_methods(false)
