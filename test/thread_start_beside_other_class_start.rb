# Thread.start is Thread.new by another name. A program that reopens Thread
# for an instance method of its own and defines `start` as a class method
# of another class (a server's) still starts a thread: neither is a class
# method of Thread's.
class Thread
  def tag = self[:tag]
end
class Clock
  def self.start = :clock
end
class Timer
  class << self
    def fork = :timer
  end
end
th = Thread.start(2) { |n| n * 21 }
th[:tag] = :t
p th.value
p th.tag
p Thread.fork { :forked }.value
p Clock.start
p Timer.fork
