# An Array holding instances of the class that holds it stays an object
# Array (sp_PtrArray) when the method walking it ends in the walk and no
# caller reads that method's value; pop and shift keep the type as well.
class Node
  attr_reader :kids, :name
  def initialize(name); @name = name; @kids = []; end
  def add(n); @kids << n; n; end
  def walk; @kids.each { |k| k.walk }; end
  def count
    t = 1
    @kids.each { |k| t += k.count }
    t
  end
  def names(out)
    out << @name
    @kids.each { |k| k.names(out) }
    out
  end
  def drop_last; @kids.pop; end
  def drop_first; @kids.shift; end
end

root = Node.new("root")
a = root.add(Node.new("a"))
a.add(Node.new("a1"))
a.add(Node.new("a2"))
root.add(Node.new("b"))
root.walk
puts root.count
puts root.names([]).join(",")
x = root.drop_last
puts x.name
y = root.drop_first
puts y.name
puts root.kids.size
z = root.drop_last
p z.nil?
p root.drop_first.nil?
puts root.count

# a walk whose value IS read stays on the boxed path, and stays correct
class Tree
  def initialize; @items = []; end
  def add(t); @items << t; self; end
  def visit; @items.each { |i| i.visit }; end
end
t = Tree.new
t.add(Tree.new).add(Tree.new)
r = t.visit
p r.size
p t.visit.length

# pop / shift on a local object Array: empty answers nil, a frozen one raises
class Job
  attr_reader :id
  def initialize(id); @id = id; end
end
q = [Job.new(1), Job.new(2), Job.new(3)]
first = q.shift
puts first.id
last = q.pop
puts last.id
q << Job.new(4)
while (j = q.shift)
  puts j.id
end
p q.empty?
p q.pop.nil?
p q.shift.nil?
f = [Job.new(5)]
f.freeze
begin
  f.pop
rescue => e
  puts e.class
end
begin
  f.shift
rescue => e
  puts e.class
end
p f.size
