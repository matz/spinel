# spinel: reject-unsupported: `...` forwarded to `new` on a class value
# `...` forwarded to `new` on a class value stays refused when the value
# may hold a Struct class through `itself`: the Class value's
# itself and tap answer the receiver, so the class facts keep the record
# class.
class Mid
  def initialize(app, opt: 1) = (@app = app; @opt = opt)
end
class B
  def initialize = @use = []
  def use(m, ...) = @use << proc { |app| m.new(app, ...) }
  def to_app(app) = @use.reverse.inject(app) { |a, e| e[a] }
end
P = Struct.new(:app, :opt)
D = Data.define(:app)
b = B.new
b.use(Mid, opt: 2)
b.use(P.itself, 6)
p b.to_app(5)
