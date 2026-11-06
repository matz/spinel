# A method an included hook's `base.class_eval` defines is copied into each
# including class, and only the copies run: the module's original never runs
# as itself. A call of the class's own class method from the copy
# (`self.class.record`) binds that class's method, defined in the same block
# before it, with nothing in between to choose from at run time.
module Loggable
  def self.included(base)
    base.class_eval do
      @log = []
      def self.log = @log
      def self.record(event) = (@log << event; self)
      def note(event) = self.class.record(event)
    end
  end
end

module Tagged
  def self.extended(base)
    base.class_eval do
      def self.tag = "tag:#{name}"
    end
  end
end

class Order
  include Loggable
  extend Tagged
end

class Ticket
  include Loggable
end

Order.new.note(:a)
Order.new.note(:b)
Ticket.new.note(:c)
p Order.log, Ticket.log, Order.tag
p Order.new.respond_to?(:note)
