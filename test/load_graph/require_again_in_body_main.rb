# A require in a body (a module body, a method, a block) of a file that an
# earlier top-level require loaded runs after that load: it loads nothing
# and answers false. The file keeps its place in the load order, so its
# definitions are known where the program uses them. The graph gave the
# file no place, and `find_it` below was "unsupported".

require_relative "require_again_in_body_lib"

module Other
  p require_relative("require_again_in_body_lib")
end

class Disk
  include Auth::Lookup

  def f
    find_it
  end
end
puts Disk.new.f

def again
  require_relative "require_again_in_body_lib"
end
p again
[1].each { |_| p require_relative("require_again_in_body_lib") }

# The require in `early` stands before the top-level one, and the call of
# `early` runs it first: this file has no place.
def early
  require_relative "require_again_in_body_late_lib"
end
puts "before"
early
puts "between"
require_relative "require_again_in_body_late_lib"
puts Late.new.name
