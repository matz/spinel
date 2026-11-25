# A file the entry's first statement requires stays placed when its other
# require is in a cycle of requires (x and y require each other) that only
# a class-body require after that statement enters. The alias in it and
# the shim's redefinition are then in a known order.
require_relative "cycle_later_placed_esc_lib"
require_relative "cycle_later_placed_shim_lib"
require_relative "cycle_later_placed_provider_lib"
require_relative "cycle_later_placed_concern_lib"
p Esc.escape("<a>")
p Provider.new.debug
