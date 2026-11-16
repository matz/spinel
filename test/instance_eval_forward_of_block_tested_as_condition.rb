# A method's own block tested as a bare condition (`obj.instance_eval(&block) if block`)
# and forwarded to instance_eval on a value whose class a class value made:
# the condition asks whether a block was given, and the forward is that block.

class Base
  def state(name)
    name
  end
end

class Article
  def self.aasm(flag, &block)
    klass = flag ? Base : Base
    m = klass.new
    m.instance_eval(&block) if block
    block ? m.state(:ran) : :none
  end
  p aasm(true) { state :draft }
  p aasm(false)
end
