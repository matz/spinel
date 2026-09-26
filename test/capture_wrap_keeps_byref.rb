# The capture-wrap lambda a block gets when a proc inside it captures the
# block's parameter is called where it is made and returns inside the
# iteration, so the cells it captures do not outlive the call. Counted as
# outliving, a String parameter it captured lost the by-reference ABI and
# every append landed in a copy the caller never saw (#5087).
# Any class defining a yielding `filter` makes every `x.filter { }` block
# count as a proc for the capture-wrap desugar.
class Rel
  def initialize(items) = @items = items
  def filter
    out = []
    @items.each { |x| out << x if yield(x) }
    out
  end
end

def show_into(io, stories, comments)
  stories.each do |s|
    io << s << ":" << comments.filter { |c| c == s }.size.to_s << " "
  end
  nil
end

def show(stories, comments)
  io = +""
  show_into(io, stories, comments)
  io
end

puts show(["a", "b"], ["a", "a", "b"])
p Rel.new([1, 2]).filter { |x| x > 1 }

# a proc made inside the wrapped body that does escape still keeps its own
# captures
def collect(xs)
  procs = []
  xs.each do |x|
    procs << -> { x * 10 }
    Rel.new([x]).filter { |y| y == x }
  end
  procs.map(&:call)
end
p collect([1, 2, 3])
