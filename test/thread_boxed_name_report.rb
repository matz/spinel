# Thread#name and #report_on_exception on a Thread read back out of an
# Array or a boxed instance variable, beside a typed Thread, a Class and a
# user class that answer name, and respond_to? on a boxed Thread.
class Worker
  def name = "worker"
end

class Pool
  def initialize(t) = @t = t
  def thread = @t
end

t = Thread.new { :done }
t.name = "stream 3"
t.report_on_exception = false
t.join(5)
u = Thread.new { 1 }
u.join(5)

boxed = [t, 1][0]
p boxed.name
p boxed.report_on_exception
p [u, 1][0].name
p [u, 1][0].report_on_exception
[t, Worker.new, String].each { |x| p x.name }
Pool.new(1)
pool = Pool.new(t)
p pool.thread.name
p pool.thread.report_on_exception
p t.name, t.report_on_exception
p %i[name report_on_exception value join alive? key? [] kill].map { |m| boxed.respond_to?(m) }
