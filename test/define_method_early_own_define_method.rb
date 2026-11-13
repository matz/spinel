# An options object with an accessor named define_method (a config object
# answers any name through method_missing, and a build that writes those
# accessors out names this one too). The program's own define_method was read
# as the one every receiverless define_method reaches, and the method
# `Agg.step` defines as one the class might answer already: the
# `step_with_ctx` call below then made that define_method a refusal. An
# accessor of a Hash answers no call whose self is the class Agg: the call
# reaches Module#define_method, and the method is registered from the start
# as any define_method in a method body is.
class Opts < Hash
  def define_method = self[:define_method]
end

class Agg
  def self.step = define_method(:step_with_ctx) { |ctx, x| ctx << x }
  def step(ctx, x) = step_with_ctx(ctx, x)
end

Agg.step
ctx = []
Agg.new.step(ctx, 1)
Agg.new.step(ctx, 2)
p ctx
o = Opts.new
o[:define_method] = :an_option
p o.define_method
