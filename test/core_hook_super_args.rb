# A super from a hook that reaches only the core hook (Class#inherited,
# Module#method_added) evaluates its arguments, and a count other than
# one raises ArgumentError as the core hook does.
class Base
  def self.log(k)
    puts "log #{k}"
    k
  end
  def self.inherited(k)
    super(log(k))
  end
end
class Sub < Base; end

class Two
  def self.inherited(k)
    super(k, 1)
  rescue ArgumentError => e
    puts "Two: #{e.message}"
  end
end
class TwoSub < Two; end

class Zero
  def self.inherited(k)
    super()
  rescue ArgumentError => e
    puts "Zero: #{e.message}"
  end
end
class ZeroSub < Zero; end

class Splat
  def self.inherited(k)
    a = [k]
    r = super(*a)
    p r
  end
end
class SplatSub < Splat; end

class Plain
  def self.inherited(k)
    p super
  end
end
class PlainSub < Plain; end

module Tracker
  def self.method_added_log(n) = (puts "added #{n}"; n)
end
class Watch
  def self.method_added(n)
    super(Tracker.method_added_log(n))
  end
  def foo = 1
end
puts "done"
