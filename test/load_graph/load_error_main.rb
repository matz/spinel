# A require that names no file compiles and raises
# CRuby's LoadError when it runs, wherever it stands: a `rescue LoadError`
# fallback runs, the message names the feature, and LoadError#path answers
# it (a require_relative's from the file's own directory).
def setup
  require "load_error_no_such_lib"
  :loaded
rescue LoadError => e
  [:fallback, e.message, e.path]
end
p setup

HAVE_LIB = begin
  require "load_error_no_such_lib"
  true
rescue LoadError
  false
end
p HAVE_LIB

x = begin
  require("load_error_other_lib") ? :loaded : :not
rescue ScriptError => e
  e.class
end
p x

def last = require("load_error_last_lib")
begin
  last
rescue LoadError => e
  p e.message
end

begin
  require_relative "load_error_dir/../load_error_nope"
rescue LoadError => e
  p e.path.start_with?("/"), e.path.end_with?("/load_graph/load_error_nope")
  p e.message == "cannot load such file -- #{e.path}"
end

p LoadError.new("by hand").path
puts "done"
