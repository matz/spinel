# send(m, &block) over an ivar-held name whose arms include a String
# iterator: its &block forward lowers to a nil test that sets a temp, and
# every other arm (any, tag, map) read that temp, unset there, and got no
# block.
class MethodCall
  def initialize(m)
    @method_name = m
  end

  def make_lambda
    lambda do |target, value, &block|
      target.send(@method_name, &block)
    end
  end
end

class Collector
  def any(*args, &block)
    block ? block.call(args.size) : :none
  end
end

class Builder
  def tag(&block)
    block ? "<#{block.call}>" : "<>"
  end
end

out = []
l = MethodCall.new(:each_byte).make_lambda
l.call("ab", 1) { |b| out << b }
p out
p l.call("ab", 1).class
p MethodCall.new(:each_char).make_lambda.call("xy", 0) { |ch| out << ch }
p out
p MethodCall.new(:any).make_lambda.call(Collector.new, 2) { |n| n + 10 }
p MethodCall.new(:any).make_lambda.call(Collector.new, 2)
p MethodCall.new(:tag).make_lambda.call(Builder.new, 2) { "b" }
p MethodCall.new(:tag).make_lambda.call(Builder.new, 2)
p MethodCall.new(:upcase).make_lambda.call("ab", 3)
p MethodCall.new(:size).make_lambda.call([1, 2, 3], 3)
p MethodCall.new(:map).make_lambda.call([1, 2, 3], 3) { |x| x * 2 }
