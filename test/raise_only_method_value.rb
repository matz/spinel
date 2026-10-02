# A method whose every path raises answers no value, and the analyzer types it
# void. Compared with `==` or interpolated, its call was refused at compile
# time ("unsupported equality", "unsupported interpolation value"), so a base
# class with `def version = raise NotImplementedError` stopped a program CRuby
# runs, even where the comparison sits in a method no live path reaches.
# Evaluating the call raises, as in CRuby, and the argument is never evaluated.

class Migration
  def version = raise(NotImplementedError, "subclass")
end

class Migrator
  def initialize(ms) = @ms = ms
  def lines = @ms.map { |m| "v#{m.version}" }
  def has?(v) = @ms.any? { |m| m.version == v }
  def other?(v) = @ms.any? { |m| m.version != v }
end

mg = Migrator.new([Migration.new])
[:lines, :has?, :other?].each do |what|
  begin
    what == :lines ? mg.lines : mg.send(what, "1")
  rescue NotImplementedError => e
    puts "#{what}: #{e.message}"
  end
end

# a receiver of the class itself, and a bare call to a top-level method
m = Migration.new
begin
  p(m.version == "1")
rescue NotImplementedError => e
  puts "typed: #{e.message}"
end

def oops = raise(ArgumentError, "top")
begin
  p(oops == 3)
rescue ArgumentError => e
  puts "bare ==: #{e.message}"
end
begin
  puts "x#{oops}y"
rescue ArgumentError => e
  puts "bare interpolation: #{e.message}"
end

# the argument is not evaluated once the receiver raises
def noisy
  puts "argument evaluated"
  "1"
end
begin
  p(m.version == noisy)
rescue NotImplementedError
  puts "argument skipped"
end

# a method that returns nil, a bare puts, is not one of them
p(puts("said") == nil)

# nor is a method whose body is one: it is typed nil, not void, and returns
def log = puts("logged")
p(log == nil)
puts "x#{log}y"
class Logger2
  def info = print("info\n")
end
p(Logger2.new.info == nil)
