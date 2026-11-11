# spinel: not-cruby -- CRuby answers each call below; Spinel has no route for it and raises NotImplementedError where it runs
# A call of a method CRuby gives its receiver, that no route of the compiler
# takes, raises NotImplementedError where it runs, naming the CRuby method
# (the compile warns of it). The receiver and the arguments run first, in
# order, as CRuby evaluates them before the method. NotImplementedError is
# a ScriptError, so `rescue NameError` and `rescue NoMethodError` do not
# catch it.
def trace(s)
  puts "eval #{s}"
  s
end

def attempt(label)
  yield
  puts "#{label}: answered"
rescue NotImplementedError => e
  puts "#{label}: #{e.class}: #{e.message}"
end

class K
  def im = private_methods(trace(false))
  def self.cm = local_variables
  attempt("class body") { class_variables }
end

attempt("main") { local_variables }
attempt("main global") { global_variables }
attempt("instance method") { K.new.im }
attempt("class method") { K.cm }
attempt("explicit Integer receiver") { 5.singleton_methods(trace(false)) }
attempt("builtin class value") { String.instance_methods(trace(false)) }
attempt("module value") { Comparable.constants }
attempt("module function") { Kernel.global_variables }
attempt("send") { send(:local_variables) }

begin
  begin
    local_variables
  rescue NameError
    puts "caught as NameError"
  end
rescue NotImplementedError => e
  puts "not a NameError: #{e.class}"
end

begin
  begin
    5.singleton_methods
  rescue NoMethodError
    puts "caught as NoMethodError"
  end
rescue NotImplementedError => e
  puts "not a NoMethodError: #{e.class}"
end

# A builtin method a program class makes public, which CRuby still answers
# with the builtin's
class Pub; public :global_variables; end
attempt("made public") { Pub.new.global_variables }

# A gap in an argument of a call a program class also defines raises where
# the argument runs, for an Array as for the program's class
class Cycler; def cycle(n) = yield(n); end
cy = ARGV.size > 5 ? Cycler.new : [1, 2]
attempt("operand of a builtin arm") { cy.cycle(global_variables.size > 0 ? 2 : 1) { |s| s } }

# What the refusals of such a call used to pin: a block that may be nil
# handed on through a program `new`'s super, a bare instance_method in a
# reopened builtin class, and a typed Hash's blockless transform_values.
def least(a, &b) = a.min(&b)
class A
  attr_reader :r
  def initialize(a, &b) = @r = least(a, &b)
end
xs = ARGV.size > 5 ? "x" : [3, 1, 2]
attempt("block through new") { p A.new(xs) { |x, y| y <=> x }.r }
class B < A; def self.new(a) = super(a, &nil); end
attempt("nil block through new's super") { p B.new(xs).r }
class String
  attempt("bare instance_method") { instance_method(:upcase) }
end
h = { a: 1, b: 2 }
attempt("typed hash") { h.transform_values }

# A lookup by a literal name that is there is the gap too, and so is one the
# record cannot prove absent (an alias adds it). The names that are nowhere
# raise CRuby's NameError (test/unrouted_builtin_name_lookup.rb).
attempt("name that is there") { Integer.instance_method(:puts) }
class Integer; alias_method :zork, :to_s; end
attempt("name an alias adds") { Integer.instance_method(:zork) }

# ...and one a library the program loads gives the class (the json methods
# a require adds): there whatever the program defines, never a NameError
require "json"
attempt("package method") { Integer.instance_method(:to_json) }
require "pp"
attempt("pp method") { Integer.instance_method(:pretty_inspect) }

# A gap in a block that a builtin's arm of a dispatch splices leaves the arm
# in place: an Array's each_slice, beside a program class's own, still runs
# and raises from the block
class Slicer
  def each_slice(n)
    yield [n]
    self
  end
end
def slicer(i) = i > 0 ? Slicer.new : [1, 2, 3, 4]
sl = slicer(ARGV.size)
attempt("gap in a spliced block") { sl.each_slice(2) { |x| puts "slice"; global_variables } }
slicer(1).each_slice(3) { |x| puts "slice" }

# ...and so does a String's iterator beside a program class's: the String
# runs the block, which raises
class Graphemes
  def each_grapheme_cluster
    yield "g"
    self
  end
end
def graphemes(i) = i > 0 ? Graphemes.new : "ab"
attempt("gap in a String iterator's block") { graphemes(ARGV.size).each_grapheme_cluster { |g| puts g; global_variables } }

# A boxed `&` argument is taken as a proc before the gap raises, as CRuby
# converts it before the method runs, and is read once: nil, a Symbol, a
# Proc, a Method and a Hash pass, an Array, a Range and a plain object raise
# CRuby's TypeError
def blk_arg(i)
  puts "pick #{i}"
  [nil, :to_s, proc { }, method(:puts), { a: 1 }, [1, 2], 1..2, Object.new][i]
end
8.times do |i|
  attempt("block argument #{i}") { 5.singleton_methods(&blk_arg(i)) }
rescue TypeError => e
  puts "block argument #{i}: TypeError: #{e.message}"
end

# A class value's call names the method its class side holds, as CRuby's
# lookup finds it ahead of Class's or Module's: Class.constants is
# Module.constants, a method apart from Module#constants
attempt("class side of Module") { Class.constants }
attempt("Module's own") { Module.constants }

# ...and names the class side's own method for a class's own class
# method: Class.allocate (CRuby answers a class) and Complex.polar with no
# argument (CRuby's ArgumentError, which the gap stands in for) stay the gap
attempt("Class's own allocate") { Class.allocate }
attempt("Complex.polar with no argument") { Complex.polar }
