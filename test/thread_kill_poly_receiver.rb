# Thread#kill raised NoMethodError unless the receiver was a local whose
# assignment spinel could trace: a handle taken out of an Array -- or held in
# another object's ivar, which is how a shutdown path keeps it -- reached the
# poly dispatch, which had arms for #join, #alive? and #status but none for
# #kill, so the call compiled to a raise (#4619). A Fiber answers it too, as
# it does #alive?.
$stdout.sync = true

threads = Array.new(3) { Thread.new { sleep 30 } }
sleep 0.2
threads.each(&:kill)
threads.each(&:join)   # (waits for the kill: a sleep did not on a loaded runner)
puts "after each(&:kill): #{threads.count(&:alive?)} alive"

more = Array.new(3) { Thread.new { sleep 30 } }
sleep 0.2
more.each { |t| t.kill }
more.each { |t| t.join }
puts "after each { t.kill }: #{more.count(&:alive?)} alive"

# The ivar shape: the handle lives in another object, as a connection's
# owning thread does.
class Holder
  def initialize(thread)
    @thread = thread
  end

  def stop
    @thread&.kill
  end

  def running? = @thread.alive?
  def wait = @thread.join
end

holder = Holder.new(Thread.new { sleep 30 })
sleep 0.2
holder.stop
holder.wait
puts "after ivar kill: #{holder.running?}"

local = Thread.new { sleep 30 }
sleep 0.2
local.kill
local.join
puts "after local kill: #{local.alive?}"
