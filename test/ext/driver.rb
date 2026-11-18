# Drives the generated CRuby extension (ext-cruby-test); output pinned in
# expected_cruby.
# GC stress before the extension first allocates: a collection then runs
# inside every argument conversion, not only on a large enough input.
ENV["SPINEL_GC_STRESS"] = "1"
$LOAD_PATH.unshift(File.dirname(__FILE__))
require "extk"
p ExtKernel.triple(5)
p ExtKernel.shout("hey")
[[false, false], [true, false], [false, true]].each do |binary, missing|
  value = ExtKernel.sample(binary, missing)
  p value && [value.bytes, value.encoding.name]
end
p ExtKernel.total([1, 2, 3])
begin
  ExtKernel.must_pos(-2)
rescue ArgumentError => e
  puts "ArgumentError: #{e.message}"
end
begin
  ExtKernel.triple("x")
rescue TypeError
  puts "TypeError"
end
p ExtKernel.must_pos(6)
left = Array.new(400) { |i| "left-#{i}" }
right = Array.new(400) { |i| "right-#{i}" }
want = left.sum(&:length) + right.sum(&:length)
p 20.times.all? { ExtKernel.pair_sum(left, right) == want }
# A raise after an argument was rooted -- converting the second argument, an
# element inside its helper, or the kernel's own -- leaves the root stack as
# it found it: the calls after it collect and still answer.
def at_depth(n, &b) = n.zero? ? b.call : at_depth(n - 1, &b)
def raises?(klass)
  yield
  false
rescue klass
  true
end
p(20.times.all? do |i|
  raises?(TypeError) { ExtKernel.pair_sum(left, 1) } &&
    raises?(TypeError) { ExtKernel.pair_sum(left, ["x", 2]) } &&
    raises?(ArgumentError) { ExtKernel.pair_sum([], right) } &&
    at_depth(i % 7) { ExtKernel.pair_sum(left, right) } == want
end)
# NOTE: TOPLEVEL_NOTE deliberately absent -- the kernel's toplevel runs on
# the SPINEL side at init; nothing but the entry methods exists on the host.

# The subprocess bounds a native deadlock: a Ruby Timeout cannot run if
# a caller waits for the extension gate while holding the GVL.
require "rbconfig"
pid = Process.spawn(RbConfig.ruby, "-I", File.dirname(__FILE__), "-e", <<~'RUBY')
  require "extk"
  GC.start
  GC.compact if GC.respond_to?(:compact)
  def native(thread)
    Thread.pass until thread.status == "sleep" || !thread.alive?
    raise "call did not overlap" unless thread.alive?
  end

  owner = Thread.new { ExtKernel.pause_total([1, 2, 3], 0.5) }
  native(owner)
  raise "wrong concurrent result" unless ExtKernel.triple(7) == 21
  raise "lost array roots" unless owner.value == 6

  threads = 4.times.map do
    Thread.new do
      20.times do
        raise "wrong array result" unless ExtKernel.pair_sum(["ab", "c"], ["def"]) == 6
      end
    end
  end
  threads.each(&:value)

  owner = Thread.new { ExtKernel.pause_total([1, 2, 3], 0.5) }
  native(owner)
  waiter = Thread.new { ExtKernel.triple(2) }
  native(waiter)
  waiter.kill.join
  raise "owner failed after waiter kill" unless owner.value == 6

  owner = Thread.new do
    begin
      ExtKernel.pause_total([1, 2, 3], 0.5)
    rescue RuntimeError => error
      error.message
    end
  end
  native(owner)
  owner.raise(RuntimeError, "interrupted")
  raise "owner interrupt lost" unless owner.value == "interrupted"
  raise "gate or roots leaked" unless ExtKernel.pair_sum(["ab"], ["c"]) == 3

  invalid = Object.new
  def invalid.to_f = raise(TypeError, "conversion failed")
  begin
    ExtKernel.pause_total([1, 2, 3], invalid)
    raise "conversion did not raise"
  rescue TypeError => error
    raise "wrong conversion error" unless error.message == "conversion failed"
  end
  recursive = Object.new
  def recursive.to_f
    ExtKernel.triple(2)
    0.001
  end
  begin
    ExtKernel.pause_total([1, 2, 3], recursive)
    raise "recursive entry did not raise"
  rescue ThreadError
  end
  raise "conversion roots or gate leaked" unless ExtKernel.pair_sum(["ab"], ["c"]) == 3
RUBY
deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 20
status = nil
until status || Process.clock_gettime(Process::CLOCK_MONOTONIC) >= deadline
  result = Process.waitpid2(pid, Process::WNOHANG)
  status = result.last if result
  sleep 0.01 unless status
end
unless status
  Process.kill(:KILL, pid)
  Process.waitpid(pid)
  abort "concurrent extension calls timed out"
end
abort "concurrent extension calls failed" unless status.success?
p true
