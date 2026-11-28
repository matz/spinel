# spinel: gc-stress
# spinel: share
# Writing an exception subclass instance into a TY_EXCEPTION slot upcasts
# to sp_Exception * without losing class identity, ivars, or exception methods.

class NotFound < StandardError; end

class Coded < StandardError
  attr_reader :code, :tags

  def initialize(msg, code, tags = [])
    super(msg)
    @code = code
    @tags = tags
  end

  def label
    "#{@code}:#{@tags.join(',')}"
  end
end

class SubCoded < Coded
  attr_reader :detail

  def initialize(msg, code, detail)
    super(msg, code, [detail])
    @detail = detail
  end
end

module Mod
  class ScopedErr < StandardError
    attr_reader :origin

    def initialize(msg, origin)
      super(msg)
      @origin = origin
    end
  end
end

class CustomMsg < StandardError
  def initialize(msg, prefix)
    super(msg)
    @prefix = prefix
  end

  def message
    "[#{@prefix}] #{super}"
  end

  def to_s
    "to_s(#{@prefix})"
  end
end

# 1. ivar-less subclass written to rescue => e
begin
  raise "x"
rescue => e
end
e = NotFound.new("gone")
puts "1: #{e.class}: #{e.message}"

# 2. ivar-bearing subclass with heap ivars, exception methods, dup/clone/freeze, raise
begin
  raise "x"
rescue => e2
end
e2 = Coded.new("missing", 404, ["http", "get"])
puts "2: #{e2.class}: #{e2.message}: #{e2.code}: #{e2.label}: #{e2.inspect}"
puts "2a: isa=#{e2.is_a?(Coded)},#{e2.is_a?(StandardError)},#{e2.is_a?(RuntimeError)} iof=#{e2.instance_of?(Coded)},#{e2.instance_of?(StandardError)}"
case e2
when Coded then puts "2b: case=Coded"
else puts "2b: case=other"
end
d = e2.dup
puts "2c: dup=#{d.class}:#{d.message}:#{d.code}:#{d.label}:same=#{d.equal?(e2)}"
e2b = e2.exception("changed")
puts "2d: exc=#{e2b.class}:#{e2b.message}:#{e2b.code}:#{e2b.label}"
e2.set_backtrace(["app.rb:10:in 'run'"])
puts "2e: bt=#{e2.backtrace.inspect} ivget=#{e2.instance_variable_get(:@code)}"
e2.freeze
puts "2f: frozen=#{e2.frozen?} clone=#{e2.clone.frozen?} dup=#{e2.dup.frozen?}"
begin
  raise e2
rescue Coded => caught
  puts "2g: raised=#{caught.class}:#{caught.message}:#{caught.code}:#{caught.label}"
end

# 3. multi-level and module-scoped subclasses
begin
  raise "x"
rescue => e3
end
e3 = SubCoded.new("deep", 503, "retry")
puts "3a: #{e3.class}: #{e3.message}: #{e3.code}: #{e3.detail}: #{e3.is_a?(Coded)}"
e3 = Mod::ScopedErr.new("scoped", "db")
puts "3b: #{e3.class}: #{e3.message}: #{e3.origin}: #{e3.is_a?(Mod::ScopedErr)}"

# 4. conditional write keeps TY_EXCEPTION methods like full_message(highlight: false)
def check_cond
  begin
    raise "orig"
  rescue => e
  end
  e = NotFound.new("gone") if block_given?
  fm = e.full_message(highlight: false)
  "#{e.class}:#{e.message}:#{fm.include?(e.message)}"
end
puts "4a: #{check_cond}"
puts "4b: #{check_cond { 1 }}"

# 5. write before rescue, closure capture, expression-position write, return as TY_EXCEPTION
def return_coded
  e = NotFound.new("pre")
  pre = "#{e.class}:#{e.message}"
  begin
    raise "mid"
  rescue => e
  end
  rd = -> { "#{e.class}:#{e.message}" }
  mid = rd.call
  x = (e = Coded.new("post", 201, ["ok"]))
  puts "5: pre=#{pre} mid=#{mid} cap=#{rd.call} x=#{x.class}:#{x.code}"
  e
end
rc = return_coded
puts "5r: #{rc.class}:#{rc.message}:#{rc.code}:#{rc.label}"

# 6. overridden message / to_s
begin
  raise "x"
rescue => e6
end
e6 = CustomMsg.new("boom", "T1")
puts "6: msg=#{e6.message} to_s=#{e6.to_s}"

# 7. retry and ensure
tries = 0
seen = nil
begin
  tries += 1
  raise "t#{tries}" if tries < 2
rescue => e7
  e7 = NotFound.new("r#{tries}")
  seen = "#{e7.class}:#{e7.message}"
  retry
ensure
  puts "7: #{seen}"
end
