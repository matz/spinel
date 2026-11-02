# A require in a rescue clause that never runs loads nothing there, but
# where the program has a require of a computed path, that require may load
# the same file: the file stays part of the program.
begin
  require_relative "rescue_open_defs_lib"
rescue LoadError
  require_relative "rescue_open_fb_lib"
end
name = ENV.fetch("SPINEL_RESCUE_OPEN_NO_SUCH_VAR", "fb")
require_relative "rescue_open_#{name}_lib"
p RescueOpenFb.hi
