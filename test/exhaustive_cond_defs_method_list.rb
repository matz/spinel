# A def under an `if` whose every branch, the else included, defines the
# same name is defined whichever way the condition goes once the body has
# run. A list of methods (public_methods, instance_methods and their kin)
# anywhere in the program is then not refused on its account, as it is for
# a def only one branch of a condition makes.
class Event
  if GC.respond_to?(:total_time)
    def now_gc = GC.total_time
  else
    def now_gc = 0
  end
  if GC.stat.key?(:total_allocated_objects)
    def now_allocations = GC.stat(:total_allocated_objects)
  else
    def now_allocations = 0
  end
  def name = "event"
end
class Response
  if [].respond_to?(:bsearch_index)
    def status_code = 422
  elsif [].respond_to?(:bsearch)
    def status_code = 421
  else
    def status_code = 420
  end
end
class Base
  def common = 1
end
class Subscriber < Base
  def self.subscriber = @subscriber
  def self.subscriber=(s); @subscriber = s; end
  def self.fetch_public_methods(subscriber, inherit_all)
    subscriber.public_methods(inherit_all) - Base.public_instance_methods(true)
  end
  def start = 1
end
Subscriber.subscriber = Subscriber.new
p Subscriber.fetch_public_methods(Subscriber.subscriber, false).sort
p Event.public_instance_methods(false).sort, Event.new.respond_to?(:now_gc), Event.new.now_allocations.is_a?(Integer)
p Response.instance_methods(false), Response.new.status_code
