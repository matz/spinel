# A class whose initialize always raises still makes objects where it
# defines `new` itself (allocate): the computed require in its method runs,
# and the file it loads is placed as its candidate, so the method the file
# defines is there before the fixed require of it below.
require_relative "computed_require_allocated_class/source"
RubySource.new.load_index("index")
p index_loaded
require_relative "computed_require_allocated_class/index"
p index_loaded
