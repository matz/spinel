# A superclass's class method calls, bare, a private class method that only
# a subclass defines; a subclass of that subclass calls the superclass's
# method too, so the subclass's method gets a copy specialized for it. The
# subclass's own copy stays: the superclass method's copy for the subclass
# calls it. Before, it was taken for unused and dropped, and the emitted C
# named a function that was never defined.
class Subscriber
  def self.debug_methods = @dm_set ? @dm : superclass.debug_methods
  def self.debug_methods=(val); @dm_set = true; @dm = val; end
  @dm = []; @dm_set = true
  def self.attach = (debug_only(:start); debug_methods)
end
class EventSubscriber < Subscriber
  class << self
    def attach_to = attach
    private
      def debug_only(method)
        self.debug_methods = self.debug_methods + [method]
      end
  end
end
class Other < EventSubscriber; end
p EventSubscriber.attach_to
p Other.attach_to
p EventSubscriber.debug_methods, Other.debug_methods, Subscriber.debug_methods
