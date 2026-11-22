# Receiverless mixins in the class_eval block of an included hook: extend
# (whose extended hook sets the includer's ivar), include (with its own
# included hook) and prepend are the including class's own, each made
# where the block's statements run.
module BwTracker
  def self.extended(base)
    puts "tracker on #{base.name}"
    base.instance_variable_set(:@tracked, true)
  end
  def tracked? = @tracked
  def kind = "tracked"
end
module BwHelpers
  def self.included(base) = puts("helpers in #{base.name}")
  def help = "help from #{self.class.name}"
end
module BwFirst
  def greet = "first"
end
module BwConcern
  def self.included(base)
    base.class_eval do
      extend BwTracker
      include BwHelpers
      prepend BwFirst
      def greet = "own"
    end
  end
end
class BwA
  include BwConcern
end
class BwB
  include BwConcern
  def greet = "b"
end
p BwA.tracked?, BwA.kind, BwA.new.help, BwA.new.greet, BwB.new.greet
p BwA.ancestors.take(4), BwB.ancestors.take(4)
