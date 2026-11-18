# A recursive writer returns the handle of its growing String. Returning
# from each call must take constant time in the length already written.
class Node
  attr_reader :type, :children

  def initialize(type, children)
    @type = type
    @children = children
  end
end

class Writer
  attr_reader :out

  def initialize
    @out = +""
  end

  def write(node)
    if node.nil?
      @out << "nil"
      return
    end
    @out << "(" << node.type.to_s
    node.children.each do |c|
      @out << " "
      if c.is_a?(Node)
        write(c)
      elsif c.is_a?(String)
        string(c)
      else
        @out << c.to_s
      end
    end
    @out << ")"
  end

  def string(s)
    @out << "\""
    i = 0
    while i < s.bytesize
      @out << s.getbyte(i).chr
      i += 1
    end
    @out << "\""
  end
end

def build(d)
  return Node.new(:leaf, ["ab", nil, 5]) if d == 0

  Node.new(:send, [build(d - 1), build(d - 1)])
end

w = Writer.new
w.write(build(17))
puts w.out.bytesize
puts w.out[0, 60]
puts w.out[-30, 30]

# The same shapes where a caller reads the value: each still answers it.
class Log
  def initialize
    @buf = +""
  end

  def add(x)
    @buf << x << ";"
  end

  def add_twice(x)
    add(x)
    add(x)
  end
end

log = Log.new
log.add("a")
v = log.add("b")
puts v
puts log.add_twice("c")

# a read through a subclass's super
class Base
  def initialize
    @s = +""
  end

  def put(x)
    @s << x
  end
end

class Derived < Base
  def put(x)
    super(x.upcase)
  end
end

d = Derived.new
d.put("p")
puts d.put("q")

# a read through an is_a? guard on a value of either class
def label(o)
  o.is_a?(Log) && o.add("g")
end

puts label(log)
p label(1)
