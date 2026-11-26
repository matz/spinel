# `...` forwarded to `new` on a class value whose classes take keywords in
# initialize builds, in a program with a Struct and a Data class that the
# value never holds (Rack::Builder#use).
class Mid
  def initialize(app, opt: 1) = (@app = app; @opt = opt)
  def call(env) = [@opt, @app.call(env)]
end
class Tag
  def initialize(app, name = "t", &blk) = (@app = app; @name = blk ? blk.call(name) : name)
  def call(env) = [@name, @app.call(env)]
end
class B
  def initialize = @use = []
  def use(middleware, ...)
    @use << proc { |app| middleware.new(app, ...) }
  end
  def to_app(app) = @use.reverse.inject(app) { |a, e| e[a] }
end
P = Struct.new(:x)
D = Data.define(:y)
b = B.new
b.use(Mid, opt: 2)
b.use(Mid)
b.use(Tag, "u") { |s| s.upcase }
p b.to_app(->(env) { env }).call(5)
p P.new(1).x, D.new(y: 2).y
