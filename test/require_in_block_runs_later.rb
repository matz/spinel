# A require inside a block or a method runs the file's own statements when
# the require runs, not at program start: minitest requires a file from its
# `-W` option block whose body would otherwise run in every program.
$log = [:main]
class Parser
  def initialize = @blocks = []
  def on(&b) = @blocks << b
  def parse = @blocks.map(&:call)
end
def options(o)
  o.on do
    r = require_relative "require_in_block_runs_later/opt"
    [r, Opt::Hook.on]
  end
end
def unused
  require_relative "require_in_block_runs_later/never"
  Never.v
end
o = Parser.new
options(o)
p $log
p o.parse
p $log
p o.parse
p $log
p require_relative("require_in_block_runs_later/opt")
p $log
# a file a method requires, then required again at the top level
def late = require_relative("require_in_block_runs_later/late")
require_relative "require_in_block_runs_later/late" if true
p __LINE__
p $log
p late
# a file whose statement raises loads again on the next require
$tries = []
def boom = require_relative("require_in_block_runs_later/boom")
x = (boom rescue "raised")
p x, $tries
p boom, $tries
# a statement that reads what a deferred one did keeps its order
def reg = require_relative("require_in_block_runs_later/reg")
p reg, ITEMS
