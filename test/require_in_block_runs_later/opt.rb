module Opt
  module Hook
    def self.on = "hook on"
  end
  $log << :opt_module_body
end
puts "opt top level"
