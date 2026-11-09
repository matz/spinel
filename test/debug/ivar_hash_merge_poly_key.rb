# Built with --debug: `@h = @h.merge(k => v).freeze` on a class-level Hash
# ivar, in a program where a Hash carries instance variables. The ivar's
# nil test reads the held receiver after the argument Hash is built (it
# went ahead of the statement and named a temp not yet declared).
class Hash
  def tag!(v)
    @tag = v
    self
  end

  def tag = @tag
end

class Tracker
  @trackers = {}

  def self.register(k, v)
    if @trackers.frozen?
      @trackers = @trackers.merge(k => v).freeze
    else
      @trackers[k] = v
    end
  end

  def self.trackers = @trackers
end

Tracker.register(:a, 1)
Tracker.trackers.freeze
Tracker.register("b", 2.5)
p Tracker.trackers
p Tracker.trackers.frozen?
p({}.tag!(1).tag)
