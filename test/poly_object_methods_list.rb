# obj.public_methods / methods / singleton_methods on a receiver that may be
# an instance of several classes of the program: each class's list, picked
# by the object's class at run time, as the typed receiver's static list is.
# A subscriber base class lists a subscriber's public methods to attach
# them, the subscriber being whichever subclass registered.
class Subscriber
  def self.subscriber = @subscriber
  def self.subscriber=(v); @subscriber = v; end
  def self.attach_to(inherit_all)
    fetch_public_methods(subscriber, inherit_all)
  end
  def self.fetch_public_methods(subscriber, inherit_all)
    subscriber.public_methods(inherit_all) - Subscriber.public_instance_methods(true)
  end
  def start = 0
end
class ViewSub < Subscriber; def render = 1; end
class CtlSub < Subscriber
  def process = 2
  def redirect = 3
  protected def guarded = 4
end
ViewSub.subscriber = ViewSub.new
p ViewSub.attach_to(false), ViewSub.attach_to(true)
CtlSub.subscriber = CtlSub.new
p CtlSub.attach_to(false).sort, CtlSub.attach_to(true).sort
class Boom < StandardError; end
begin; raise Boom; rescue Boom => e; p e.class; end
def pick(n) = n.zero? ? ViewSub.new : CtlSub.new
p pick(0).methods(false).sort, pick(1).methods(false).sort
p pick(1).public_methods(false).sort, pick(0).singleton_methods
