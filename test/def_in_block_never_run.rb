# A def in a block defines its method when the block runs. A block a class
# body hands to a method that never uses its block does not run, so what it
# holds defines nothing; a block that runs defines its methods as before.
module Concern
  def class_methods(&block) = nil
end
module Gate
  extend Concern
  class_methods do
    def per_page = 50
  end
  def gate = :gate
end
class C
  include Gate
end
puts "#{C.new.respond_to?(:per_page)} #{Gate.respond_to?(:per_page)} #{C.new.gate}"
p Gate.instance_methods, Gate.method_defined?(:per_page), C.new.methods.include?(:per_page)
begin
  C.new.per_page
rescue NoMethodError => e
  puts e.message
end

# the class's own class method, on self and by a superclass
class Own
  def self.skip; end
  skip { def a = 1 }
  self.skip { def b = 2 }
  def c = 3
end
class Sub < Own
  skip { def d = 4 }
end
p Own.instance_methods(false), Sub.instance_methods(false), Sub.new.c

# a module that extends itself, and a Class.new block
module Selfish
  extend self
  def skip(*) = :skipped
  skip(1) { def a = 1 }
end
Made = Class.new do
  def self.skip; end
  skip { def a = 1 }
  def b = 2
end
p Selfish.instance_methods(false), Made.instance_methods(false)

# everything such a block holds
module Mixed; def mixed = 1; end
class Forms
  def self.skip; end
  def orig = 1
  skip do
    attr_reader :q
    define_method(:dm) { 1 }
    alias_method :al, :orig
    include Mixed
    K = 1
    def self.cm = 1
    def a = 1
  end
end
f = Forms.new
p [:q, :dm, :al, :mixed, :a].map { |m| f.respond_to?(m) }
p Forms.respond_to?(:cm), Forms.const_defined?(:K), Forms.include?(Mixed), Forms.instance_methods(false)

# the method the block would have replaced answers
class Kept
  def self.skip; end
  def a = 10
  skip { def a = 50 }
end
p Kept.new.a

# the call under a condition only the run time decides, or in an expression:
# the block cannot run whether or not the call does
module Guarded
  extend Concern
  if ENV["HOME"]
    class_methods do
      def per_page = 50
    end
  end
end
class Placed
  include Guarded
  def self.skip(*) = nil
  if ARGV.empty?
    skip { def in_if = 1 }
  else
    skip { def in_else = 1 }
  end
  skip { def mod_if = 1 } if ARGV.empty?
  skip { def mod_unless = 1 } unless ARGV.size > 3
  case ARGV.size
  when 0 then skip { def in_when = 1 }
  else skip { def in_case_else = 1 }
  end
  begin
    skip { def in_begin = 1 }
  ensure
    skip { def in_ensure = 1 }
  end
  skip { def rescued = 1 } rescue nil
  ARGV.empty? && skip { def anded = 1 }
  x = skip { def assigned = 1 }
  y = [skip { def in_array = 1 }, 2]
  p x, y
end
p Placed.new.respond_to?(:per_page), Placed.instance_methods(false)

# a block that runs defines its methods: yielded to, called, handed on
class Runs
  def self.yields; yield; end
  def self.calls(&b) = b.call
  def self.hands(&b) = yields(&b)
  def self.anon(&) = yields(&)
  def self.rest(...) = yields(...)
  def self.bound(&b) = binding.local_variable_get(:b).call
  yields { def a = 1 }
  calls { def b = 2 }
  hands { def c = 3 }
  anon { def d = 4 }
  rest { def e = 5 }
  bound { def f = 6 }
end
r = Runs.new
p Runs.instance_methods(false).sort, [r.a, r.b, r.c, r.d, r.e, r.f]

# one of the name's methods uses its block: a subclass's own, a super's, a
# version a method body or a lambda defines before the call
class Base; def self.cfg; end; def self.sup; yield; end; end
class Over < Base
  def self.cfg; yield; end
  def self.sup; super; end
  cfg { def a = 1 }
  sup { def b = 2 }
end
class Later
  def self.late; end
  def self.fix!
    def self.late; yield; end
  end
  fix!
  late { def a = 1 }
end
class Lam
  def self.lam; end
  L = -> { class ::Lam; def self.lam; yield; end; end }
  L.call
  lam { def a = 1 }
end
class Single
  def self.single; end
  define_singleton_method(:single) { |&b| b.call }
  single { def a = 1 }
end
p Over.instance_methods(false).sort, Later.new.a, Lam.new.a, Single.new.a

# a name the program takes away somewhere may be left to method_missing,
# which runs the block
class Stripped
  def self.gone; end
  def self.method_missing(name, *args, &blk) = blk ? blk.call : super
  def self.respond_to_missing?(*) = true
  def self.strip!
    class << self
      remove_method :gone
    end
  end
  strip!
  gone { def a = 1 }
end
p Stripped.instance_methods(false)

# a name the language reads its own way keeps that reading: the block of a
# `loop` is left in, whatever the class defines by that name
class Poller
  def self.loop = :mine
  loop { break }
end
puts "polled"

# the blocks the language runs
class Lang
  [1].each { def a = 1 }
  class_eval { def b = 2 }
  instance_eval { def c = 3 }
end
Mod = Module.new do
  def d = 4
end
St = Struct.new(:x) do
  def e = 5
end
class Lang; include Mod; end
l = Lang.new
p l.a, l.b, Lang.c, l.d, St.new(1).e
