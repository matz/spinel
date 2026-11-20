# An array that holds objects of one class narrows to a pointer array, which
# keeps only an object's pointer. A boxed value that is no object of the
# program (a String, a number, a Symbol), pushed or index-stored into the
# array, keeps it boxed, and reads back as what went in.

class Tag; end

def pick(n) = [1].size > n ? 3 : "a"

nodes = []
nodes << Tag.new
nodes << pick(5)
nodes.each { |x| p x.class }

floats = []
floats << Tag.new
floats << ([1].size > 5 ? 3 : 4.5)
floats.each { |x| p x.class }

later = []
later << pick(5)
later << Tag.new
later.each { |x| p x.class }

class Body
  def initialize = @nodes = []
  attr_reader :nodes

  def parse(toks)
    toks.each do |t|
      @nodes << Tag.new
      @nodes << t
    end
  end
end

b = Body.new
b.parse([1].size > 5 ? 3 : ["s"])
b.nodes.each { |x| p x.class }

stored = [Tag.new]
stored[0] = ([1].size > 5 ? 3 : "s")
stored.push([1].size > 5 ? :sym : 2.5)
stored.each { |x| p x.class }
