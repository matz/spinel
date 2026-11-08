# A Method of a boxed receiver lists the parameters of the method its
# receiver's class has, as its arity already answers by that class. The
# receiver is boxed when it may be of several classes (here a Proc or an
# object), and the Method then stands on a wrapper taking the receiver and
# a rest: its #parameters listed the wrapper's, [[:req, :__bam_r], [:rest,
# :__bam_a]]. activesupport's Notifications pick a subscriber's kind by
# `listener.method(:call)`'s arity and parameters length.
class Sub
  def call(event) = event
end

class Two
  def call(a, b = 1, *rest, k:, &blk) = a
end

class Holder
  attr_reader :call
end

def kind(listener)
  procish = listener.respond_to?(:parameters) ? listener : listener.method(:call)
  [procish.arity, procish.parameters, procish.arity == 1 && procish.parameters.length == 1]
end

p kind(Sub.new)
p kind(Two.new)
p kind(Holder.new)
p kind(proc { |e| e })
p kind(proc { |*a| a })
p kind(lambda { |x, y| x })

def meth_of(o) = o.method(:call)   # a Sub, a Two or a Holder
[Sub.new, Two.new, Holder.new].each { |o| m = meth_of(o); p [m.arity, m.parameters] }

# a builtin receiver beside a class of the program: the builtin's list
class Pair
  def first(n = 1) = n
end
def first_of(o) = o.method(:first)
[Pair.new, [1, 2], 1..3].each { |o| m = first_of(o); p [m.arity, m.parameters] }
