# `self.a op= v` (and `self.a ||= v`) in a class method, where `a` and `a=`
# are class methods (a class-level attribute written as defs): the reader
# and the writer run on the class the method was called on, a subclass
# included, as `self.a = self.a op v` does. Before, the write went to the
# class the method was written in, or was refused in a nested class with a
# superclass.
module Notes
  class Subscriber
    @__ca_debug_methods = ([]); @__ca_debug_methods_set = true; def self.debug_methods = @__ca_debug_methods_set ? @__ca_debug_methods : superclass.debug_methods; def self.debug_methods=(val); @__ca_debug_methods_set = true; @__ca_debug_methods = val; end
    def self.cache = @cache
    def self.cache=(v); @cache = v; end
    def self.remember(k)
      self.cache ||= {}
      self.cache[k] = true
      cache.keys
    end
  end
  class EventSubscriber < Subscriber
    class << self
      def attach_to
        debug_only(:start)
        debug_methods
      end
      private
        def set_silenced
          @silenced = debug_methods.to_h { |m| ["#{m}.evt", true] }
        end
        def debug_only(method)
          self.debug_methods += [method]
          set_silenced
        end
    end
    def self.silenced = @silenced
  end
  class Other < EventSubscriber; end
end
p Notes::EventSubscriber.attach_to, Notes::EventSubscriber.silenced
p Notes::Other.attach_to, Notes::Subscriber.debug_methods, Notes::Other.debug_methods
p Notes::Other.remember(:x), Notes::Other.remember(:y), Notes::Subscriber.cache, Notes::EventSubscriber.cache

class Counter
  @__ca_n = 0; @__ca_n_set = true; def self.n = @__ca_n_set ? @__ca_n : superclass.n; def self.n=(v); @__ca_n_set = true; @__ca_n = v; end
  def self.bump = self.n += 1
end
class Sub < Counter; end
Counter.bump; Sub.bump; Sub.bump
p Counter.n, Sub.n
