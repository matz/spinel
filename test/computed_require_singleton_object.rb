# a file loaded after a method with a computed require gives an Object.new
# constant a singleton method (Minitest's UNDEFINED in assertions.rb): the
# singleton class the compiler makes for it is no constant to guard
def load_plugin(path) = require(path)
require_relative "computed_require_singleton_object/plugin"
p UNDEFINED
p Opts.make.topdict
