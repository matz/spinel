# A file two top-level requires load, the first of which runs before a
# require of it in a method body: the method is defined, and so called,
# only after that first load, so the later require loads nothing and the
# file keeps its place. Unplaced, its initialize was taken as the one the
# class ends with though the reopening below runs after it.
require_relative "nested_require_after_first_dispatch"
require_relative "nested_require_after_first_base"
require_relative "nested_require_after_first_lib"

def lazy_base
  require_relative "nested_require_after_first_base"
  Base.new.go
end

p lazy_base.discard
