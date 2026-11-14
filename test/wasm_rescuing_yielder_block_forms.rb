# On wasm32 a method of the program that yields and has a rescue, an ensure
# or a rescue modifier takes its block as a proc (`&blk`, `blk.call`) rather
# than being copied into each caller; what the block does must not change:
# break, next, return, a write to the caller's local, a raise, more than one
# yield, a yield in a loop, a retry, and a method that asks block_given? (it
# keeps the copied form).
# spinel: wasm
module Batch
  def self.run
    begin
      r = yield 1
      [:done, r]
    rescue => e
      [:rescued, e.message]
    ensure
      puts "ensure"
    end
  end

  def self.pair(a, b)
    x = yield(a, b)
    y = yield(b, a)
    [x, y]
  rescue ArgumentError
    :bad
  end

  def self.each_safe(xs)
    out = []
    xs.each do |v|
      begin
        out << yield(v)
      rescue ZeroDivisionError
        out << :div
      end
    end
    out
  end

  def self.quiet
    (yield) rescue :quiet
  end

  def self.again
    tries = 0
    begin
      tries += 1
      yield tries
    rescue RuntimeError
      retry if tries < 3
      :gave_up
    end
  end

  def self.maybe
    return :no_block unless block_given?
    begin
      yield
    rescue
      :rescued
    end
  end
end

class Caller
  def brk = Batch.run { |x| break :broke }
  def nxt = Batch.run { |x| next x + 10 }
  def ret; Batch.run { |x| return :returned }; :after; end
  def loc; v = 0; Batch.run { |x| v = x + 41 }; v; end
  def rai = Batch.run { |x| raise "boom" }
  def me = Batch.run { |x| self.class }
end

c = Caller.new
p c.brk
p c.nxt
p c.ret
p c.loc
p c.rai
p c.me
p Batch.pair(1, 2) { |a, b| a * 10 + b }
p Batch.pair(1, 2) { |a, b| raise ArgumentError if a > b; a }
p Batch.each_safe([1, 0, 4]) { |v| 12 / v }
p Batch.quiet { 7 }
p Batch.quiet { raise "x" }
p Batch.again { |t| raise "again" if t < 2; t }
p Batch.again { |t| raise "always" }
p Batch.maybe
p Batch.maybe { 5 }
p Batch.maybe { raise "y" }
