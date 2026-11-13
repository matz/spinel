# The load record's open histories (--dump-load-record,
# record_open_main.rb.load-record): a history is open when a version after
# its last surely applied one stands under a run-time condition in its
# class body, or in an iterator's block or a begin body with rescue
# clauses past a statement that may raise, so the method the load phase
# ends with is the last version that ran. The line of an open history ends
# with ` open`.
#
# Open: a def under a run-time condition, a def in an iterator's block,
# a def in a begin body with a rescue clause after a call.
# Not open: a def under a constant condition (folded), a def in a class
# body opened under a condition (the condition is the body's), a def a
# begin body with a rescue clause starts with, and a history a sure def
# ends after an open stretch.
class Gate
  if ARGV.empty?
    def run_time = :run_time
  end
  [1].each { def from_block = :block }
  begin
    def first_in_begin = :first
    Integer("1")
    def from_begin = :begin
  rescue ArgumentError
    nil
  end
  def folded = :folded if true
  def twice = 1 if ARGV.empty?
  def twice = 2
end

# A class body opened under a condition: its own defs are the class's for
# good, a def under another condition in it is open.
if ARGV.empty?
  class Framed
    def framed = :framed
    if ARGV.empty?
      def inner = :inner
    end
  end
end

# A may version dominates the points after it in its branch, and those in
# a body nested there: the aliases and the call there take the version,
# which the alias after the branch and the one in the else arm cannot know.
class Branch
  if ARGV.empty?
    def self.tag = :tag
    p tag
    define_method(:hi) { :hi }
    alias hello hi
    [1].each { alias hey hi }
  else
    alias other hi
  end
  alias late hi
end

g = Gate.new
p g.run_time, g.from_block, g.first_in_begin, g.from_begin, g.folded, g.twice
f = Framed.new
p f.framed, f.inner
b = Branch.new
p b.hi, b.late
