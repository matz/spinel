# a lambda with keyword parameters inside a block a method stores and runs
# later: the lambda's keywords are its own, not outer variables the block
# failed to capture (Rails' error reporter middleware in an initializer)
class Reg
  def initialize; @mw = []; end
  def add(m) = @mw << m
  def run(ctx) = @mw.map { |m| m.call(RuntimeError.new("x"), handled: true, severity: :warning, context: ctx, source: "s") }
end
REG = Reg.new
$inits = []
def initializer(name, **opts, &blk) = $inits << blk

initializer :err, group: :all do |app|
  REG.add(->(error, handled:, severity:, context:, source:) {
    context.merge(app: app, severity: severity, handled: handled, error: error.message, source: source)
  })
  REG.add(->(error, **rest, &blk) { rest.keys.sort })
end
$inits.each { |b| b.call(:myapp) }
p REG.run({ a: 1 })
