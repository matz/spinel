# A proc a class method returns is called by its caller through the boxed
# ABI, so its parameters are boxed (#3175). A class method a subclass
# calls, and that reads the subclass's own state, runs as a copy made
# for the subclass, whose body is a clone no def holds: the rule marking a
# def's tail literal as escaping never saw the clone's, and its
# parameters took the Integer default. A Hash argument then raised
# TypeError at `event[:name]`, and `to_s` printed the pointer.
# activesupport's LogSubscriber.subscription_filter is this method.
class LogSubscriber
  class << self
    attr_accessor :namespace
  end

  def self.subscription_filter
    prefix = "#{namespace}."
    proc { |event| event[:name].start_with?(prefix) }
  end

  def self.describer
    tag = namespace
    lambda { |x, y| "#{tag}: #{x.inspect} #{y.inspect}" }
  end

  def self.explicit
    n = namespace
    return proc { |v| [n, v] }
  end
end

class AcLog < LogSubscriber
  self.namespace = "ac"
end

class AvLog < LogSubscriber
  self.namespace = "av"
end

f = AcLog.subscription_filter
p f.call({ name: "ac.start" })
p f.call({ name: "av.render" })
p AvLog.subscription_filter.call({ name: "av.render" })
p AcLog.describer.call({ a: 1 }, "s")
p AvLog.describer.call([1, 2], 3.5)
p AcLog.explicit.call("x")
p AvLog.explicit.call(:y)

# an instance method an include copies into its class
module Filtering
  def filter_for(pre) = proc { |e| e[:name].start_with?(pre) }
end
class Sub
  include Filtering
end
p Sub.new.filter_for("ac").call({ name: "ac.x" })
