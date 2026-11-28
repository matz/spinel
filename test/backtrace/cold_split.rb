# A -g build at -O2: the C compiler may split a method's raising path off
# into a piece of its own. clang's `sp_Spike_s_inner.cold.1` is a function
# the method calls, so one Ruby frame has two C frames; gcc's `.cold` is the
# method's own blocks in another section, with no frame of its own. Either way
# the backtrace names the method once per call, without the piece's suffix
# (backtrace-test). `inner` calls itself, so at least one frame of it stays a
# real function; how many of its calls keep a frame at -O2 is the C
# compiler's choice, so the test counts no frames, it only wants no frame
# named twice in a row. (macOS clang splits this one into `.cold.N` pieces.)
module Spike
  def self.outer(n) = middle(n)
  def self.middle(n) = inner(n)

  def self.inner(n)
    raise ArgumentError, "boom #{n}" if n > 1
    return n if n < 0

    inner(n + 1) + n
  end
end

begin
  Spike.outer(1)
rescue ArgumentError => e
  puts e.backtrace
end
