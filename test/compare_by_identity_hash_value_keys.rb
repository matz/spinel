# Hash#compare_by_identity keyed by what a Hash holds as a value, under a
# String key or the answer of a store; a pattern's capture of a Struct's or a
# Data's member. None of them is the String beside it.
Sig = Struct.new(:name, :params)
Call = Struct.new(:args, :info)
Pos = Data.define(:line, :sig)

class Checker
  attr_reader :concrete

  def initialize
    @sigs = {}
    @concrete = []
    @queue = []
  end

  def declare(name)
    sig = Sig.new(name, [])
    r = (@sigs[name] = sig)
    enqueue(r)
    r
  end

  def enqueue(sig) = @queue << sig

  def method_sig(name) = @sigs[name.to_s]

  def run
    @concrete << @queue.shift until @queue.empty?
  end
end

class Owner
  def initialize(c)
    @c = c
    @summary = {}.compare_by_identity
  end

  def check
    @c.concrete.each { |sig| note(sig) }
    @summary.size
  end

  def note(sig)
    @summary[sig] ||= 0
    @summary[sig] += 1
  end

  def called(name)
    sig = @c.method_sig(name)
    @summary[sig] ||= 0
    @summary[sig] += 10
  end

  def walk(node)
    case node
    in Call[args:, info:]
      @summary[info] ||= 0
      @summary[info] += args.size
    in Pos[line:, sig:]
      @summary[sig] ||= 0
      @summary[sig] += line
    else
      nil
    end
  end

  def count(sig) = @summary[sig]
end

c = Checker.new
a = c.declare("a")
b = c.declare("b")
c.run
o = Owner.new(c)
p o.check
o.called("a")
o.walk(Call.new([1, 2], b))
o.walk(Pos.new(line: 5, sig: a))
o.walk(3)
p o.count(a), o.count(b), o.count(Sig.new("a", []))
