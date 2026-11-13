# A class of the program's own with pop, shift, deq, push, enq and << taking
# keywords, beside boxed values that are a Queue, a SizedQueue, an Array or
# a String: the dispatch gives each its own answer to the timeout: forms.
def t(label)
  r = yield
  puts "#{label}: #{r.inspect}"
rescue => e
  puts "#{label}: #{e.class}: #{e.message}"
end

class Mailbox
  def initialize = @log = []
  def pop(timeout: nil) = [:pop, timeout]
  def shift(timeout: nil) = [:shift, timeout]
  def deq(flag = false, timeout: nil) = [:deq, flag, timeout]
  def push(x, flag = nil, **opts) = (@log << [:push, x, flag, opts]; self)
  def enq(x, **opts) = (@log << [:enq, x, opts]; self)
  def <<(x, **opts) = (@log << [:<<, x, opts]; self)
  def log = @log
end

q = Thread::Queue.new
t("Thread arg Mailbox") { Thread.new(Mailbox.new) { |w| w.pop(timeout: 0.5) }.value }
t("Thread arg Queue") { Thread.new(q) { |w| w.pop(timeout: 0.01) }.value }
q << :item
t("Thread arg Queue, an item") { Thread.new(q) { |w| w.pop(timeout: 0.01) }.value }

box = Mailbox.new
full = Thread::SizedQueue.new(1)
full << 0
[box, q, full, [1, "a"], "s"].each do |v|
  name = v.class
  t("#{name} pop") { v.pop(timeout: 0.01) }
  t("#{name} shift") { v.shift(timeout: 0.01) }
  t("#{name} deq(false)") { v.deq(false, timeout: 0.01) }
  t("#{name} push") { r = v.push(1, timeout: 0.01); r.equal?(v) ? :self : r }
  t("#{name} push(false)") { r = v.push(2, false, timeout: 0.01); r.equal?(v) ? :self : r }
  t("#{name} enq") { r = v.enq(3, timeout: 0.01); r.equal?(v) ? :self : r }
  t("#{name} <<") { r = v.<<(5, timeout: 0.01); r.equal?(v) ? :self : r }
  begin
    v.push(4, timeout: 0.01)
  rescue => e
    puts "#{name} push statement: #{e.class}: #{e.message}"
  end
end
p box.log
p full.size
