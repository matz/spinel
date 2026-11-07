# spinel: int64 -- Integer#size of a fixnum is 8, as on a 64-bit CRuby
# A boxed builtin whose class has no #size or #length raises NoMethodError.
# The boxed size/length helpers read any object they had no arm for as 0.
class C
  def send(d) = "C #{d}"
end

def pick(i) = [C.new, Time.at(0).utc, 7, "ab", /re/, ->(a) { a }, method(:puts),
               Mutex.new, Random.new(1), Rational(1, 2), Complex(1, 2),
               StandardError.new("m"), Object.new, Thread.current, Process.times][i]

15.times do |i|
  v = pick(i)
  a = (v.size rescue $!.class)
  b = (v.length rescue $!.class)
  s = (v.send(:size) rescue $!.class)
  puts "#{i} #{a.inspect} #{b.inspect} #{s.inspect}"
end

# A reopened builtin's own size still runs.
class Fiber
  def size = 33
end
class MyErr < StandardError
  def size = 5
  def length = 6
end
def pick2(i) = [Fiber.new { 1 }, MyErr.new("x"), [1, 2], { a: 1 }, nil][i]
5.times do |i|
  v = pick2(i)
  puts "#{(v.size rescue $!.class).inspect} #{(v.length rescue $!.class).inspect}"
end

# empty? on the same builtins, a Process::Tms and any Range raises too.
def pick3(i) = [Time.at(0), /re/, Object.new, Process.times, 1..3, "a".."c",
                Time.at(0)..Time.at(9), "ab", [], {}][i]
10.times do |i|
  v = pick3(i)
  p [(v.empty? rescue $!.class), (v.size rescue $!.class), (v.length rescue $!.class)]
end

# A Range of Times without a begin names NilClass in the TypeError.
def pick4(i) = [(..Time.at(5)), (Time.at(0)..), (Time.at(0)..Time.at(5)), "ab"][i]
4.times do |i|
  v = pick4(i)
  p [(v.size rescue [$!.class, $!.message]), (v.length rescue $!.class)]
end
