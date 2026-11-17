# spinel: gc-stress
# The message of the exception a thread died with is a root of the thread
# until a join or value re-raises it. A thread kept the message beside its
# exception object and marked the object only; the message is the copy a
# raise puts on the exception stack, not the object's own, so a collection
# between the thread's death and the join freed it, and the join re-raised
# freed bytes.
Thread.report_on_exception = false
boom = Thread.new { raise "thread failed" }
Thread.pass while boom.alive?
junk = Array.new(2000) { |i| "s#{i}" * 4 }
begin
  boom.join
rescue => e
  puts "#{e.class}: #{e.message}"
end
begin
  boom.value
rescue => e
  puts "#{e.class}: #{e.message}"
end
p junk.size

