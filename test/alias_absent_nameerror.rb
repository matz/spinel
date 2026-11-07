# An alias or alias_method of a method the class does not have where it runs
# raises NameError there ("undefined method 'm' for class 'K'", 'module' for a
# module, class '#<Class:K>' for a singleton) and makes no method: the later
# def does not satisfy it, respond_to? and instance_methods(false) leave it
# out, and the statements around it run in order. An original a builtin
# answers (Object's, Kernel's, a builtin superclass's, a Struct's member, a
# builtin module's) or a method the program defined before it is no error.
# the original is defined later in the class body
class Later
  begin
    alias n m
  rescue NameError => e
    puts e.message
  end
  def m = 1
  alias_method :o, :m
  alias_method :ok, :m
  begin
    alias_method "s", "nope"
  rescue NameError => e
    puts e.message
  end
end
p Later.new.respond_to?(:n), Later.new.respond_to?(:o), Later.new.ok
p Later.instance_methods(false).sort

# an accessor made later is no method yet
class AccessorLater
  begin
    alias rr r
  rescue NameError => e
    puts e.message
  end
  attr_reader :r
  attr_writer :w
  begin
    alias_method :ww, :w2=
  rescue NameError => e
    puts e.message
  end
end
p AccessorLater.new.respond_to?(:rr), AccessorLater.instance_methods(false).sort

# a module's, and the class that includes it
module Mixin
  begin
    alias_method :n, :nope
  rescue NameError => e
    puts e.message
  end
  def m = 1
  alias_method :ok, :m
end
class Includer
  include Mixin
  begin
    alias_method :k, :nope2
  rescue NameError => e
    puts e.message
  end
end
p Includer.new.respond_to?(:n), Includer.new.respond_to?(:ok), Includer.new.respond_to?(:k)
p Mixin.instance_methods(false).sort

# a parent that defines it only later, and a sibling that does
class Parent; end
class Child < Parent
  begin
    alias_method :n, :m
  rescue NameError => e
    puts e.message
  end
end
class Parent
  def m = 1
end
class Child
  alias_method :ok, :m
end
class Sibling < Parent
  def only_here = 2
end
class Parent
  begin
    alias_method :q, :only_here
  rescue NameError => e
    puts e.message
  end
end
p Child.new.respond_to?(:n), Child.new.ok, Parent.new.respond_to?(:q)

# the singleton side
class Single
  def self.m = 1
  class << self
    alias n m
    begin
      alias_method :bad, :nope
    rescue NameError => e
      puts e.message
    end
    begin
      alias bad2 inst_only
    rescue NameError => e
      puts e.message
    end
  end
  def inst_only = 2
end
p Single.n, Single.respond_to?(:bad), Single.singleton_methods.sort

# an alias of an alias that was refused
class Chain
  begin
    alias a nope
  rescue NameError => e
    puts e.message
  end
  begin
    alias b a
  rescue NameError => e
    puts e.message
  end
end
p Chain.instance_methods(false)

# a method that only method_missing answers is no method
class Ghost
  def method_missing(n, *a) = 42
  begin
    alias_method :g, :ghost
  rescue NameError => e
    puts e.message
  end
end
p Ghost.new.respond_to?(:g), Ghost.instance_methods(false)

# a statement that does not run raises nothing, and the order of the rest holds
class Order
  puts 1
  alias n nope if false
  if false
    alias_method :o, :nope
  end
  begin
    alias n zz
  rescue NameError => e
    puts e.message
  end
  puts 3
  def zz = 4
  alias_method :n, :zz
  puts 5
end
p Order.new.n, Order.new.respond_to?(:o)

# a module_function section, a reopened builtin, the top level
module Section
  module_function
  begin
    alias_method :n, :nope
  rescue NameError => e
    puts e.message
  end
  def a = 1
end
p Section.respond_to?(:n), Section.a
class Array
  begin
    alias_method :foo, :nope
  rescue NameError => e
    puts e.message
  end
  alias_method :cnt, :size
end
p [1, 2].cnt, [].respond_to?(:foo)
begin
  alias top_n nope
rescue NameError => e
  puts e.message
end

# no error: a method defined before, a private one, a builtin's
class Fine
  def m = 1
  alias n m
  attr_reader :r
  alias rr r
  private def hidden = 7
  alias_method :shown, :hidden
  def call = shown
  alias_method :insp, :inspect
  alias_method :eq, :equal?
  alias_method :init2, :initialize
end
p Fine.new.n, Fine.new.respond_to?(:rr), Fine.new.call
class FineHash < Hash
  alias_method :ks, :keys
  alias sz size
end
h = FineHash.new
h[:a] = 1
p h.ks, h.sz
FineStruct = Struct.new(:a, :b)
class FineStruct
  alias second b
  alias_method :a2=, :a=
  begin
    alias_method :third, :c
  rescue NameError => e
    puts e.message
  end
end
p FineStruct.new(1, 2).second, FineStruct.new(1, 2).respond_to?(:third)
module Prepended
  def pm = :p
end
module Counted
  include Comparable
end
class FineMixins
  prepend Prepended
  include Counted
  include Enumerable
  alias_method :n, :pm
  alias_method :lt, :<
  alias_method :each2, :each_slice
  def each = yield(1)
  def <=>(o) = 0
end
p FineMixins.new.n

# a constant that holds a Struct, subclassed; Kernel's methods the program
# does not implement (CRuby's own method tables answer); a module's singleton;
# a module included into Object; the reopened Class, named by its Ruby name
StructConst = Struct.new(:a)
class StructSub < StructConst
  alias b a
  alias_method :v, :values
end
p StructSub.new(4).b
class KernelNames
  alias_method :op, :open
  alias_method :pc, :putc
  alias_method :ct, :caller_locations
end
module Math
  class << self
    alias_method :root, :sqrt
  end
end
p Math.root(9.0)
module Helper
  def helper = 1
end
class Object
  include Helper
end
class UsesHelper
  alias h helper
end
class Class
  alias_method :construct, :new
  begin
    alias_method :x, :nope
  rescue NameError => e
    puts e.message
  end
end
p Class.new.respond_to?(:x)
