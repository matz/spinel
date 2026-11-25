# A `#<SPINEL_SOURCE>` marker moves the positions of the lines after it in
# messages only: the computed require under one in the plugins file is
# still that file's, bounded by the require that places it, so the alias
# in the esc file is still read in a known order.
require_relative "computed_in_unplaced_esc_lib"
require_relative "computed_in_unplaced_shim_lib"
require_relative "computed_in_marked_registry_lib"
require_relative "computed_in_marked_plugins_lib"
p Esc.escape("<a>")
p Esc.e("<b>")
p Plugins.load("none")
Registry.load_all!
