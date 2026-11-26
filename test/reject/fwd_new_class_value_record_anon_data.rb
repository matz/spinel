# spinel: reject-unsupported: `...` forwarded to `new` on a class value
# `...` forwarded to `new` on a class value stays refused when the value
# may hold an anonymous Data class written at the call,
# in a program with no named record class.
class Mid
  def initialize(app, opt: 1) = (@app = app; @opt = opt)
end
class B
  def initialize = @use = []
  def use(m, ...) = @use << proc { |app| m.new(app, ...) }
  def build(app) = @use.map { |pr| pr.call(app) }
end
b = B.new
b.use(Mid, opt: 2)
b.use(Data.define(:app))
p b.build(5)
