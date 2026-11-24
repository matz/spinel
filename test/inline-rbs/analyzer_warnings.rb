# Inline RBS the parser maps but the compiler cannot apply: each type names a
# class outside the program, or the annotation has no class to belong to.
# Each is reported at its comment's line and column, in the types as written,
# and nothing of it is applied: the C is the C of the same program compiled
# with --no-inline-rbs. (warnings.rb holds the ones the parser reports.)
require_relative "analyzer_warnings_part"

class Clock
  # @rbs @log: Array[UnknownTime]
  # @rbs @zone: Net::HTTP

  attr_accessor :stamp #: Regexp?

  def initialize
    @log = []
    @stamp = nil
    @zone = "UTC"
  end

  #: (Integer) -> Regexp
  def at(x)
    x
  end

  #: (Pathname) -> Integer
  def size(path)
    path.length
  end

  def zone = @zone
end

# @rbs @noted: bool
def note
  @noted = true
end

c = Clock.new
p c.at(1), c.size("abc"), c.stamp, c.zone
p note, Counts.part_count([1, 2])
