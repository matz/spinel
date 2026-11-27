# A -g build at -O2: the C compiler may split a method's raising path off
# into a piece of its own (clang's `sp_Spike_s_inner.cold.1`, gcc's `.cold`).
# The frame is still the method's, named once, without the piece's suffix
# (backtrace-test).
module Spike
  def self.outer(n) = middle(n)
  def self.middle(n) = inner(n)

  def self.inner(n)
    raise ArgumentError, "boom #{n}" if n > 0

    n
  end
end

begin
  Spike.outer(1)
rescue ArgumentError => e
  puts e.backtrace
end
