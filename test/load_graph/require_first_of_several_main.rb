# A file that only `require` statements name, several of them, loads at
# the first in the order they run, and the others load nothing, as CRuby
# runs them: here a library's own top-level require of it, then the
# program's, with a later require under a begin that is no statement of the
# order. Its definitions are placed, so an alias of one reads in order.
require_relative "require_first_of_several_lib_time"
require_relative "require_first_of_several_lib_date"
class Stamp
  alias_method :default_inspect, :inspect
  def inspect = "stamp(#{default_inspect})"
end
begin
  require_relative "require_first_of_several_lib_time"
rescue LoadError
  p :no
end
require_relative "require_first_of_several_lib_date"
p Stamp.new.inspect
p Stamp.new.when
