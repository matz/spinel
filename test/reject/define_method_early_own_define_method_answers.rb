# The program's own define_method in the class's own chain: an instance
# method's receiverless define_method reaches it, so the `step_with_ctx`
# call may find a method CRuby never defines. Still refused, as any
# define_method in a method body of a name the class may answer is (the
# accepted case, one of an unrelated class, is
# test/define_method_early_own_define_method.rb).
class Agg
  def define_method(name, &blk) = (@defs ||= []) << name
  def step = define_method(:step_with_ctx) { |ctx, x| ctx << x }
  def go(ctx, x) = step_with_ctx(ctx, x)
end
a = Agg.new
a.step
p a.instance_variable_get(:@defs)
