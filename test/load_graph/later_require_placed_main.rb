# A file the entry's first statement requires stays placed there when a
# file only a method requires also requires it: the method is defined after
# that statement, so its require runs later and loads nothing. The alias in
# the placed file and the shim's redefinition are then in a known order.
require_relative "later_require_placed_esc_lib"
require_relative "later_require_placed_shim_lib"
def load_later
  require_relative "later_require_placed_later_lib"
end
p Esc.escape("<a>")
p Esc.e("<a>")
load_later
p Later.ready
