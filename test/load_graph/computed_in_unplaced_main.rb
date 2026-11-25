# A computed require in a file only a method requires (plugins) can name
# any file, but it runs no earlier than the statement defining that method
# (setup's), after the entry's first statements: the files they require
# keep their places, and the alias in one of them is in a known order.
require_relative "computed_in_unplaced_esc_lib"
require_relative "computed_in_unplaced_shim_lib"
require_relative "computed_in_unplaced_setup_lib"
p Esc.escape("<a>")
p Esc.e("<b>")
p Setup.run
