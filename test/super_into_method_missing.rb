# A super that finds no method calls method_missing on the object with the
# method's name and arguments, and its block (Rack::Events'
# EventedBodyProxy#each and #call over BodyProxy#method_missing)
class Proxy
  def initialize(body) = @body = body
  def method_missing(name, *args, &blk)
    case name
    when :each then @body.each(&blk)
    when :call then @body.call(*args)
    else super
    end
  end
  def respond_to_missing?(name, include_all = false) = name == :each || name == :call
end

class Evented < Proxy
  def initialize(body, log)
    super(body)
    @log = log
  end
  def each
    @log << :each
    super
  end
  def call(x)
    @log << :call
    super
  end
end

log = []
seen = []
ev = Evented.new([1, 2], log)
ev.each { |v| seen << v }
p seen, log
p Evented.new(->(x) { x * 2 }, log).call(21), log

class Recorder
  def method_missing(name, *args, &blk) = [name, args, blk ? blk.call : nil]
  def respond_to_missing?(*) = true
end
class Sub < Recorder
  def pair(a, b) = super
  def swapped(a, b) = super(b, a)
  def none(a) = super()
  def with_block = super { :blk }
end
s = Sub.new
p s.pair(1, 2), s.swapped(1, 2), s.none(1), s.with_block

class OwnMissing < Recorder
  def method_missing(name, *args) = "own #{name} #{args.inspect}"
  def go(x) = super
end
p OwnMissing.new.go(3)

class Plain
  def go = super
end
begin
  Plain.new.go
rescue NoMethodError => e
  puts e.message
end

# a native class's subclass keeps the native binding as its super
require "stringio"
class Buffer < StringIO
  def method_missing(name, *args) = :missing
  def respond_to_missing?(*) = true
  def string = super()
end
p Buffer.new("abc").string
