# A bare `super` in a module's `def self.included(base)` (or extended /
# prepended) reaches Module's own hook, which does nothing. Spliced into the
# includer's body with the rest of the hook, it stood in a class body, where
# super has no method, and was refused. It is nothing there.
module Requirements
  def self.included(base)
    super
    base.extend(ClassMethods)
    base.required << :name
  end
  module ClassMethods
    def required = @required ||= []
  end
end
module Tracked
  def self.extended(base)
    super
    base.tracked = true
  end
  attr_accessor :tracked
end
class Parser
  include Requirements
  extend Tracked
end
p Parser.required, Parser.tracked
