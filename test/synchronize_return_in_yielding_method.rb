require "monitor"
LOCK = Mutex.new
MON = Monitor.new

def with_mutex
  LOCK.synchronize { return :from_mutex }
  yield
end

def with_monitor
  MON.synchronize { return :from_monitor }
  yield
end

def fetch(cache, key)
  LOCK.synchronize do
    return cache[key] if cache.key?(key)
    cache[key] = yield
  end
end

def plain
  LOCK.synchronize { return :plain }
  :after
end

begin
  p with_mutex { :yielded }
  p with_monitor { :yielded }
ensure
  puts "ensure"
end
puts "next line"

p with_mutex { :yielded }
cache = {}
p fetch(cache, :a) { 1 }
p fetch(cache, :a) { 2 }
p plain
p LOCK.locked?

t = Thread.new do
  r = with_mutex { :yielded }
  "thread: #{r.inspect}"
end
p t.join(5).value
