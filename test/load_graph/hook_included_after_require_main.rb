# An included hook a required file adds to a module fires for the includes
# after the require, not for the one before it.
module Tracked; end
class BeforeRequire; include Tracked; end
require_relative "hook_included_after_require_lib"
class AfterRequire; include Tracked; end
puts "done"
