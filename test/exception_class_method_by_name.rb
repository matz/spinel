# A rescued exception of the program's class, held in a boxed slot, answers
# #class by name alone; `exception` on that class value builds its class.
class Boom < StandardError; end
class Deep < Boom; end
class Stat < StandardError
  def initialize(n = 1)
    super("n=#{n}")
    @n = n
  end
end
class Halt < SystemExit; end
module Space
  class Inner < StandardError; end
end

def show(e)
  puts "#{e.class} #{e.message.inspect}"
end

def caught
  yield
  nil
rescue Exception => e
  e
end

# out of an Array
errs = []
begin
  raise Boom, "a"
rescue => e
  errs << e
end
begin
  raise Deep, "b"
rescue => e
  errs << e
end
begin
  raise ArgumentError, "c"
rescue => e
  errs << e
end
errs.each { |x| show x.class.exception("rebuilt") }
errs.each do |x|
  k = x.class
  show k.exception
end

# out of a method that rescues every class
[
  caught { raise Boom, "d" },
  caught { raise Deep, "e" },
  caught { raise Space::Inner, "f" },
  caught { raise Stat.new(4) },
  caught { raise RuntimeError, "g" },
  caught { raise Errno::ENOENT, "h" },
].each { |x| show x.class.exception("seven") }

held = [caught { raise Halt, "i" }, caught { raise SystemExit }]
held.each { |x| puts x.class.exception(9).status }

# the class it builds is the class it came from
built = errs.first.class.exception("again")
puts built.class
puts built.is_a?(Boom)
begin
  raise built
rescue Boom => r
  show r
end

# another class by name is still not a class of Exception
named = [Boom.new("j"), Object.new, "s", 5]
named.each do |x|
  show x.class.exception("n")
rescue NoMethodError => r
  puts r.message
end
