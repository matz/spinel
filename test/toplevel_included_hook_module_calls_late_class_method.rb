# A module an included hook copies its methods away from is also included at
# the top level, where its original `go` runs as itself. The call in `go`
# meets `Order.record` before the class defines it, so it raises there; after
# the class defines it, the call and the hook-copied `note` both bind it.
module Loggable
  def self.included(base)
    base.class_eval do
      def note(e) = self.class.record(e)
    end
  end
  def go(x) = Order.record(x)
end

class Order
  include Loggable
end

include Loggable

begin
  p go(1)
rescue NoMethodError => e
  p [:nme, e.name]
end

class Order
  def self.record(e) = [:o, e]
end

p go(2)
p Order.new.note(:b)
