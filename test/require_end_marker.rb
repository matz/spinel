# A file ends at its own `__END__` line (#9898): what follows is that
# file's data, not code, and the files and statements after the require
# are read as usual. The bytes after a required file's `__END__` were
# spliced into the program with it, and Prism stopped reading the whole
# program there; a require in any file's data section, this entry's
# included, was resolved as code. An assignment to the engine constant in
# a data section is no assignment either: the dead `RUBY_ENGINE ==
# "jruby"` branches stay dead, here and in engine.rb.
require_relative "require_end_marker/data_tail"
puts "after the first require"
require_relative "require_end_marker/heredoc"
puts helper
puts heredoc_text
loaded = require_relative "require_end_marker/value"
p loaded
p value_marker
p EndAuto.name
require_relative "require_end_marker/engine"
if RUBY_ENGINE == "jruby"
  require_relative "require_end_marker/no_such_file"
end
puts "done"
__END__
require_relative "require_end_marker/no_such_file"
require "no_such_lib_after_end"
RUBY_ENGINE = 1
