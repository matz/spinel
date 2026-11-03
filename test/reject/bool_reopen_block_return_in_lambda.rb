# A `return` in a block given to a TrueClass method, inside a lambda: the
# block runs as a proc through the method's proc form, which does not carry
# the lambda's return (CRuby prints :lam). Spinel refuses it.
class TrueClass
  def bg = yield
end
x = true
l = lambda { |q| q.bg { return :lam }; :lamlate }
p l.call(x)
