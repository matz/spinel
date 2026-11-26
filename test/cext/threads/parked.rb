require_relative 'cxl_sample'
# extension calls inside a contended Mutex: each wait parks the thread
lock = Mutex.new
counts = Array.new(4) do
  Thread.new do
    n = 0
    500.times { n += lock.synchronize { CxlSample.serialized } }
    n
  end
end.map(&:value)
# a sleep in a block that the extension calls, while another thread calls in
napper = Thread.new { CxlSample.string_callback { |text| sleep 0.05; text.size } }
caller = Thread.new { Array.new(20) { CxlSample.serialized }.sum }
p counts, napper.value, caller.value
