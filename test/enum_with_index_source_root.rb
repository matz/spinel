# Enumerator#with_index over a generator source -- a blockless Kernel#loop, or
# any enumerator that steps through a fiber -- builds a second enumerator that
# wraps the first. Building it allocates twice, and until now neither the source
# it wraps nor the small capture that carries the source and the running index
# was held in anything but a C local across those allocations.
#
# Both slots are real and neither root covers the other. The source dies at the
# capture's allocation, which is before the capture exists to hold it, and its
# `size` is then read out of freed memory. The capture dies at the wrapping
# enumerator's own allocation, and it is not a dead local at that point: it is
# stored in the new enumerator, which the collector walks on every later pass
# and whose generator reads it when the fiber first runs.
#
# Every arm builds from a source the program never names again and checks the
# pairs it yields. Measured against pristine master at the default -O2, three
# runs out of three: on a plain run the swept arm below answers 27 wrong pairs
# in twenty thousand while the other arms are clean, and under GC stress master
# segfaults outright before printing anything. So the swept arm is what carries
# this file on the plain run the suite performs; the rest are coverage for the
# stressed run and guards against the shapes coming back.

# The first arm pins both roots on a PLAIN run, which is what the suite
# performs. It sweeps the allocation phase before each construction so that some
# iterations land a collection inside the constructor with no GC stress at all.
# With both roots it answers zero; with only the capture rooted, or with neither
# root, it answers 27; with only the source rooted it segfaults. Every deletion
# fails the file, which is the point. The counts are layout-sensitive and have
# moved with the collector's budget before; which variants fail has not.
#
# It comes first, before anything is held live, because a large live set makes
# every collection walk it.

bad = 0
20000.times do |k|
  (k % 37).times { [1, 2] }
  en = loop.with_index
  bad += 1 unless en.first(2) == [[nil, 0], [nil, 1]]
end
puts "swept with_index bad=#{bad}"

bad = 0
100.times do
  en = loop.with_index
  20.times { [1, 2, 3] }
  expected = [[nil, 0], [nil, 1], [nil, 2]]
  got = en.first(3)
  bad += 1 unless got == expected
end
puts "loop.with_index live-expected bad=#{bad}"

bad = 0
100.times do
  en = loop.with_index
  120.times { [1, 2, 3, 4] }
  bad += 1 unless en.first(3) == [[nil, 0], [nil, 1], [nil, 2]]
end
puts "loop.with_index bad=#{bad}"

bad = 0
100.times do
  en = loop.with_index(5)
  120.times { [1, 2, 3, 4] }
  bad += 1 unless en.first(2) == [[nil, 5], [nil, 6]]
end
puts "loop.with_index(5) bad=#{bad}"

# driven one pair at a time through #next rather than through a fresh fiber
bad = 0
100.times do
  en = loop.with_index(2)
  120.times { [1, 2, 3, 4] }
  bad += 1 unless [en.next, en.next] == [[nil, 2], [nil, 3]]
end
puts "loop.with_index(2) next bad=#{bad}"

# the wrapping enumerator outlives the collection that follows it
bad = 0
100.times do
  en = loop.with_index
  first = en.first(1)
  120.times { [1, 2, 3, 4] }
  bad += 1 unless first == [[nil, 0]] && en.first(2) == [[nil, 0], [nil, 1]]
end
puts "loop.with_index reread bad=#{bad}"
