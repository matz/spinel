# The NoMethodError (and, for a variable-like call, the NameError) that a
# def left undefined by a run-time condition raises is built as CRuby builds
# the error of a call that finds no method: an initialize the program gives
# NoMethodError or NameError is not run, and the error carries its name,
# receiver and arguments. The program is run with an argument, so the
# conditional defs below never run. An explicit NameError.new still runs the
# program's initialize.
class NoMethodError
  def initialize(m = nil) = super("own: " + m.to_s)
end
class NameError
  def initialize(msg = nil, *rest, **kw)
    super("ownN: #{msg}", *rest, **kw)
  end
end
class Foo
  def run = tag2
end
if ARGV.empty?
  class Foo
    def tag = 1
    def take(r) = r
    def tag2 = 2
    def many(*a, **k) = a
  end
  class Fiber
    def tag = 3
  end
end

def no_method(label)
  v = yield
  puts "#{label}: #{v.inspect}"
rescue NoMethodError => e
  puts "#{label}: #{e.class} #{e.message} name=#{e.name.inspect} recv=#{e.receiver.class} args=#{e.args.inspect}"
end

def name_error(label)
  v = yield
  puts "#{label}: #{v.inspect}"
rescue NameError => e
  puts "#{label}: #{e.class} #{e.message} name=#{e.name.inspect} recv=#{e.receiver.class}"
end

no_method("no args") { Foo.new.tag }
no_method("range arg") { Foo.new.take(1..2) }
no_method("built string arg") { Foo.new.take("s" * 2) }
no_method("rest and keywords") { Foo.new.many(1, 2, k: 3) }
name_error("variable-like") { Foo.new.run }
no_method("never defined") { Foo.new.nothere }
no_method("Fiber reopening") { Fiber.new { 1 }.tag }
p NameError.new("y").message
