# Compiled with --defer-refusals: a refusal inside a Thread body abandons
# the method holding it, and the methods emitted after it are not read as
# Thread bodies: `block_given?` there still folds, so the
# `return enum_for(:m) unless block_given?` guard of a call with a block
# drops out, and the call answers the receiver, its `self` tail.
# spinel: defer-refusals: 2:start a b true a b NotImplementedError after
class Pool
  def start
    Thread.new { proc { "a" }.ruby2_keywords }
  end
end

class Exp
  def initialize(xs) = @xs = xs
  def process(&b)
    return enum_for(:process) unless block_given?
    @xs.each(&b)
    self
  end
end

puts "start"
e = Exp.new(%w[a b])
r = e.process { |x| puts x }
puts r.equal?(e)
puts e.process.to_a
begin
  Pool.new.start.join
rescue NotImplementedError => err
  puts err.class
end
puts "after"
proc { "a" }.ruby2_keywords
puts "not reached"
