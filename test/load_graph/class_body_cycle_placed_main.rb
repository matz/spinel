# A file whose class body requires a file that requires it back keeps its
# place: the require back runs while the first file loads, and loads
# nothing. The file both require (rt) keeps its place too, and the
# reopening `class X::Content` opens the class rt made.
require_relative "class_body_cycle_placed_rt_lib"
require_relative "class_body_cycle_placed_json_lib"
require_relative "class_body_cycle_placed_late_lib"
p X::Content.new.to_json
p Late.ready
