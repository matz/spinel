# A method that runs its own optional block with instance_exec, behind an
# `if block` guard, gets a Proc rather than a spliced block. A block written
# inside another such block, on the same class, is run on the receiver it is
# handed too, as the outermost one is.

class Node
  attr_reader :tag, :children

  def initialize(tag)
    @tag = tag
    @children = []
  end

  def dump = (@children.empty? ? @tag : "#{@tag}(#{@children.map(&:dump).join(",")})")
end

class View
  def emit(node)
    parent = (@open ||= []).last
    parent.children << node if parent
  end

  def build(tag, &block)
    node = Node.new(tag)
    emit(node)
    if block
      (@open ||= []).push(node)
      begin
        instance_exec(&block)
      ensure
        @open.pop
      end
    end
    node
  end

  def col(&b) = build("col", &b)

  def leaf(tag, &block)
    node = Node.new(tag)
    emit(node)
    if block && !block.parameters.empty?
      @open.push(node)
      instance_exec(node, &block)
      @open.pop
    elsif block
      @open.push(node)
      instance_exec(&block)
      @open.pop
    end
    node
  end

  def text(s) = emit(Node.new("text:#{s}"))

  def view
    build("root") do
      build("col") do
        text "a"
        build("row") { text "b" }
      end
    end
  end

  def forwarded
    build("root") do
      col do
        col { text "c" }
        leaf("x") { |n| text n.tag }
        leaf("y") { text "d" }
        leaf("z")
      end
    end
  end
end

puts View.new.view.dump
puts View.new.forwarded.dump

# guarded, without the ensure: the nested block runs on the receiver
class Log
  attr_reader :log

  def initialize(name)
    @name = name
    @log = []
  end

  def run(&b)
    instance_exec(&b) if b
    self
  end

  def note = @log << @name
end

l = Log.new("outer")
l.run { note; run { note; run { note } } }
p l.log

# a nested block run on another object of the same class has that object
# as self, not the outer block's receiver
other = Log.new("other")
outer = Log.new("outer").run { note; other.run { note } }
p outer.log
p other.log

# a block a method can both instance_exec and plainly call: the plain call
# runs on the self the block was written under
class Tap
  attr_reader :log

  def initialize(name)
    @name = name
    @log = []
  end

  def run(plain = false, &b)
    if b
      plain ? b.call : instance_exec(&b)
    end
    self
  end

  def note = @log << @name

  def pair(other) = [run(true) { note }, other.run { note }]

  def after(other)
    other.run { 1 }
    run(true) { note }
  end
end

t = Tap.new("t")
t.run { note; Tap.new("plain").run(true) { note } }
p t.log
a = Tap.new("a")
b = Tap.new("b")
a.pair(b)
p a.log, b.log

# a block that reads nothing of self still takes the receiver it is handed,
# so a later plain call does not run on that receiver
c = Tap.new("c")
d = Tap.new("d")
c.after(d)
p c.log, d.log

# a parameter default reads the receiver's ivar, not the maker's
vals = []
inner = Tap.new("inner")
Tap.new("outer").run { inner.run { |x = @name| vals << x } }
p vals

# a bare call in the block feeds its method's block value too: a nested
# block that answers another type makes the instance_exec value boxed,
# whether the method discards it or uses it
class Acc
  attr_reader :log

  def initialize(name)
    @name = name
    @log = []
  end

  def run(&b)
    instance_exec(&b) if b
    self
  end

  def val(&b)
    @log << (b ? instance_exec(&b) : nil).class
    self
  end

  def with(x, &b)
    r = instance_exec(x, &b) if b
    @log << r.class
    self
  end

  def note = @log << @name
end

class Host
  def go(acc) = acc.run { note; run { note } }
end

acc = Acc.new("a")
acc.run { note; run { note } }
acc.run { note; run { note; run { note } } }
p acc.log
vac = Acc.new("v")
vac.val { val { note }; 1 }
vac.val { val { val { :s } }; "s" }
p vac.log
wac = Acc.new("w")
wac.with(2) { |n| with(n) { |m| m * 2 }; "x" }
wac.with(3) { |n| with(n) { |m| with(m) { |k| [k] }; k = m }; n }
p wac.log
hac = Acc.new("h")
Host.new.go(hac)
p hac.log
