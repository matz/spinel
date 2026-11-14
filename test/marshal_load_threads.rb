# Marshal.load from several threads at once: each load keeps its own reader
# on the list the collector marks.
data = Marshal.dump([[1], [2]])
threads = Array.new(8) { Thread.new { 1000.times { Marshal.load(data) } } }
threads.each(&:join)
p Marshal.load(data)
