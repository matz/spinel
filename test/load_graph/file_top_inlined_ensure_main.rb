# A required file's top level that calls a method whose block holds a
# begin/ensure: the inlined body's return check is the file's, which ends
# its load, not the program's exit (whose status main's body declares).
require_relative "file_top_inlined_ensure_lib"
puts "main"
