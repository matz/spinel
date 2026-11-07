# Green threads that park at different handler depths (nested rescue,
# ensure and catch) each get their own handlers back on resume, however
# often they park: a raise after the parks lands in the right rescue, the
# ensures run in order and a throw reaches its catch.
Thread.report_on_exception = false

def nest(depth, id, log)
  if depth == 0
    3.times { sleep 0.001; Thread.pass }
    raise ArgumentError, "t#{id}" if id.even?
    throw :out, "thrown #{id}"
  end
  begin
    nest(depth - 1, id, log)
  ensure
    log << depth
  end
end

results = []
lock = Mutex.new
threads = (0...12).map do |id|
  Thread.new do
    log = []
    r = begin
      catch(:out) do
        nest(id % 5, id, log)
      end
    rescue ArgumentError => e
      "rescued #{e.message}"
    end
    lock.synchronize { results << [id, r, log] }
  end
end
threads.each(&:join)
results.sort.each { |id, r, log| puts "#{id}: #{r} #{log.inspect}" }

caught = 0
4.times do
  begin
    t = Thread.new { sleep 0.001; raise IOError, "late" }
    t.join
  rescue IOError
    caught += 1
  end
end
puts "joined raises: #{caught}"
