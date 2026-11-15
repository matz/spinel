# A builtin class's own singleton method (Time.utc, Time.now, File.join,
# Integer.sqrt, Math.sqrt) answers an explicit-receiver call ahead of a top-level def
# of the same name (Object's private method), also where the program
# reopens the builtin class. A receiver that has no such method still
# raises NoMethodError for the private top-level def.

class Time
  def reopened = 1
end
class File
  def self.reopened = 1
end
class Integer
  def reopened = 1
end
class String
  def reopened = 1
end
module Math
  def self.reopened = 1
end

def utc(y, m, d) = [y, m, d]
def now = 1
def join(a, b) = "top"
def sqrt(x) = -1
def upcase = "top"
def year = 1
def foo = 1

p Time.utc(2026, 11, 9).to_i
p Time.utc(2026, 11, 9, 12, 30).hour
p Time.now.year > 2000
p File.join("a", "b")
p Integer.sqrt(17)
p Math.sqrt(16.0)
p "x".upcase
p Time.name

def cron_like(y, m, d, h, mi) = Time.utc(y, m, d, h, mi)
p cron_like(2026, 1, 2, 3, 4).min

[-> { 5.foo }, -> { Object.new.foo }, -> { "x".foo }, -> { Time.foo },
 -> { Time.year }, -> { File.foo }, -> { Integer.foo },
 -> { Math.foo }].each do |f|
  f.call
rescue NoMethodError => e
  puts e.message.sub(/0x\h+/, "XX")
end
