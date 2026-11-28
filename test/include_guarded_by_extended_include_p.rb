# A module the class extends answers `include?` for it, through a module it
# includes too: the guard stays, as it does for the class's own include?
# (test/include_guarded_by_own_include_p.rb), and the include is skipped.
module Callbacks
  def run_callbacks(kind)
    yield
  end
end
module BasePolicy
  def include?(mod) = true   # claims every module
end
module Policy
  include BasePolicy
end
class Governed
  extend Policy
  include Callbacks unless include?(Callbacks)
end
p Governed.new.respond_to?(:run_callbacks)
p Governed.include?(Callbacks)
