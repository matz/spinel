# A rescue clause of the ScriptError family whose begin's body only
# requires files that only define things never runs: loading such a file
# raises nothing. Under --load-model=graph a require in that clause loads
# nothing, so a file only it names is not part of the program, and a
# define_method there defines nothing, as for a branch whose condition is
# false. The versioned file exists here, so its busy_timeout= is the one
# the alias takes, not the fallback's; a file the clause names that a
# method loads later is loaded there. A module mixed in by its constant
# and a require of no file give no class a definition hook, which could
# raise as such a file loads.
begin
  require_relative "rescue_never_4.0/native_lib"
rescue LoadError
  require_relative "rescue_never_native_lib"
end
require_relative "rescue_never_db_lib"
p RescueNeverDb.new.busy_timeout(5)

begin
  require_relative "rescue_never_defs_lib"
  require_relative "rescue_never_defs_lib"
rescue LoadError => e
  x = require_relative "rescue_never_unused_lib"
  p x
rescue TypeError
  p :type
end
p defined?(RescueNeverUnused), RescueNeverDefs.hi

module RescueNeverMix
  def mix = :mix
end
class RescueNeverK
  include RescueNeverMix
  begin
    require_relative "rescue_never_defs_lib"
  rescue ScriptError, NotImplementedError
    define_method(:dm) { :dm }
  end
end
p RescueNeverK.new.respond_to?(:dm)

[1, 2].each do
  begin
    require_relative "rescue_never_defs_lib"
  rescue LoadError
    begin
      require_relative "rescue_never_no_such_lib"
    rescue LoadError
      require_relative "rescue_never_unused2_lib"
    end
  end
end
p defined?(RescueNeverUnused2)

def rescue_never_later = require_relative("rescue_never_later_lib")
begin
  require_relative "rescue_never_defs_lib"
rescue LoadError
  require_relative "rescue_never_later_lib"
end
p defined?(RescueNeverLater)
rescue_never_later
p RescueNeverLater.hi
