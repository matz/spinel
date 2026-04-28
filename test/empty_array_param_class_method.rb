# Issue #58: instance method receiving an empty `[]` should follow
# the same deferred-resolution path as a top-level method.

class Recorder
  def record(buf, name)
    buf.push(name)
    buf.push(name + "!")
  end
end

r = Recorder.new
log = []
r.record(log, "go")
r.record(log, "stop")
puts log.length    # 4
puts log[0]        # go
puts log[1]        # go!
puts log[2]        # stop
puts log[3]        # stop!
