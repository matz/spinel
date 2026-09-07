# A blockless #each builds its Enumerator from the receiver it was called on,
# and keeps that receiver as the enumerator's source. The receiver is usually
# the caller's temporary -- `(11..55).each` hands it straight in and names it
# nowhere else -- and the constructor allocates before it is finished with it.
#
# Both arms of the constructor do this. The materialized arm reads the receiver
# back after allocating the enumerator, to store it as the source; the endless
# arm reads `first` and `step` out of it after allocating the capture that
# carries them, and then the capture itself has to survive allocating the
# enumerator around it. Every arm below builds from a receiver the program never
# names again, allocates between building and reading, and checks the value it
# gets rather than merely that it survived.
#
# Not every arm below fails on master the same way, and it is worth being exact,
# because the sweep at the top allocates and so shifts the phase every arm after
# it sees. Measured against pristine master at the default -O2 -- the build a
# reader gets -- on a plain run, ten runs out of ten identical:
#
#   arm                      wrong out of
#   swept first                 12   40000
#   inspect                      3     400
#   array inspect                4     400
#   endless first                1     200
#   StopIteration#result         0     400
#   endless first offset         0     200
#   endless next / peek /        0     100
#     rewind / take                    each
#
# So four arms are wrong on the plain run the suite performs, each of them a
# wrong answer with a zero exit status rather than a crash. That is what this
# file rests on, and it has been stable across every base measured.
#
# Under GC stress master fails this file too, but not reproducibly in one shape:
# it sometimes aborts before printing anything and sometimes finishes with every
# arm wrong. The counts in that mode have moved with the collector's budget and
# are not quoted here for that reason. The branch matches CRuby every run, in
# both modes.

# --- both roots, pinned separately, with no GC stress ---
#
# The arms further down need the collection to land inside the constructor, and
# on a plain build that is a matter of where the allocation phase happens to be.
# So sweep the phase: allocate k % 37 throwaway arrays before each construction
# and repeat until every offset into the collection interval has been tried.
# With both roots in place the count below is zero on a plain build. Delete the
# capture root and it reports 11; delete the receiver root and it reports 6;
# delete both and it reports 12 -- the same numbers on every run. That is what
# makes this file discriminate under the plain run the suite actually performs,
# rather than only under GC stress.
#
# This runs before anything is held live on purpose: a large live set makes
# every collection walk it, and under GC stress that costs about four times as
# long for the same sweep -- 1.2 s here against 4.3 s with it moved below KEEP.

bad = 0
40000.times do |k|
  (k % 37).times { [1, 2] }
  en = (1..Float::INFINITY).each
  bad += 1 unless en.first(3) == [1, 2, 3]
end
puts "swept first bad=#{bad}"

KEEP = (1..3000).map { |i| "s#{i}" }

def churn
  120.times { [1, 2, 3, 4] }
end

# --- the materialized arm: the receiver, read back as the enumerator's source ---

bad = 0
wrong = nil
400.times do
  en = (11..55).each
  churn
  s = en.inspect
  unless s == "#<Enumerator: 11..55:each>"
    bad += 1
    wrong ||= s
  end
end
puts "inspect bad=#{bad} wrong=#{wrong.inspect}"

# StopIteration#result is the same field read through documented behaviour
# rather than through #inspect: it answers whatever the underlying each
# returned, which for a Range#each enumerator is the range itself.
bad = 0
wrong = nil
400.times do
  en = (11..12).each
  churn
  r = nil
  begin
    en.next
    en.next
    en.next
  rescue StopIteration => ex
    r = ex.result.inspect
  end
  unless r == "11..12"
    bad += 1
    wrong ||= r
  end
end
puts "StopIteration#result bad=#{bad} wrong=#{wrong.inspect}"

# an array receiver takes the same arm. It has to come back from a method: an
# array LITERAL is held by a temporary the emitter already roots, so writing it
# inline tests nothing, while an array a method returns has no such holder and
# is exactly the temporary this constructor drops.
def make_array
  [10, 20, 30]
end

bad = 0
400.times do
  en = make_array.each
  churn
  bad += 1 unless en.inspect == "#<Enumerator: [10, 20, 30]:each>"
end
puts "array inspect bad=#{bad}"

# --- the endless arm: the range, and the capture built from it ---

bad = 0
200.times do
  en = (1..Float::INFINITY).each
  churn
  bad += 1 unless en.first(3) == [1, 2, 3]
end
puts "endless first bad=#{bad}"

# a start other than 1, so a collected range is a wrong number and not just a crash
bad = 0
200.times do
  en = (7..Float::INFINITY).each
  churn
  bad += 1 unless en.first(4) == [7, 8, 9, 10]
end
puts "endless first offset bad=#{bad}"

bad = 0
100.times do
  en = (10..Float::INFINITY).each
  churn
  bad += 1 unless [en.next, en.next, en.next] == [10, 11, 12]
end
puts "endless next bad=#{bad}"

bad = 0
100.times do
  en = (100..Float::INFINITY).each
  churn
  bad += 1 unless en.peek == 100 && en.next == 100 && en.peek == 101
end
puts "endless peek bad=#{bad}"

bad = 0
100.times do
  en = (42..Float::INFINITY).each
  churn
  en.next
  en.next
  en.rewind
  bad += 1 unless en.next == 42
end
puts "endless rewind bad=#{bad}"

bad = 0
100.times do
  en = (1..Float::INFINITY).each
  churn
  bad += 1 unless en.take(2) == [1, 2]
end
puts "endless take bad=#{bad}"

puts "keep #{KEEP.size}"
