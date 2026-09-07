# A search or in-place loop roots the receiver it reads on every turn.
#
# Each of these builtins hoists its receiver into a C temporary and reads that
# temporary again on every turn: its length is the loop bound, the element the
# block is handed comes out of it, and map!, collect! and fill write back into
# it. A receiver that no Ruby name holds is exactly the one that can be
# collected while its own loop is still walking it, so most arms below call a
# method for their receiver rather than naming it first, and their blocks
# allocate before they look at what they were handed.
#
# The exceptions are deliberate and labelled where they appear: four control
# arms hold the receiver in a local, which is itself a root, and the two arms
# that test bsearch's running answer rather than its receiver need a named
# receiver to mutate and must NOT allocate on the turn that captures the
# answer.
#
# Each arm prints the number of turns it took as well as its answer, except
# where a note says why it cannot: a loop that ends early is a changed line
# rather than a silent pass.

def make_array
  (1..40).map { |i| "a#{i}" }
end

def make_mixed
  a = []
  (1..40).each { |i| a << (i.even? ? "a#{i}" : i) }
  a
end

def make_hash
  h = {}
  (1..40).each { |i| h["k#{i}"] = "v#{i}" }
  h
end

def churn
  100.times { "q" * 64 }
end

# A halving search takes only log2(n) turns, so it needs a larger receiver and
# a block that allocates harder than the rest before a collection lands inside
# its window. These are the smallest values that still misbehave on master --
# see the pull request for what a lighter block does instead.
def make_sorted
  (1..4096).map { |i| "z%06d" % i }
end

def churn_hard
  500.times { "q" * 512 }
end

# For the two arms that test bsearch's running answer rather than its receiver.
# Whether that answer survives depends on when the block allocates, not only on
# how much: an allocation before the answer is captured promotes the string out
# of the young generation and it can no longer be collected mid-search. So
# these arms keep the capturing turn quiet and allocate only afterwards.
def churn_young
  400.times { "q" * 512 }
end

# --- search loops over a typed array ---

n = 0
r = make_array.find { |x| churn; n += 1; x == "a40" }
puts "find              turns=#{n} answer=#{r.inspect}"

n = 0
r = make_array.detect { |x| churn; n += 1; x == "a40" }
puts "detect            turns=#{n} answer=#{r.inspect}"

n = 0
r = make_array.find_index { |x| churn; n += 1; x == "a40" }
puts "find_index        turns=#{n} answer=#{r.inspect}"

n = 0
r = make_array.index { |x| churn; n += 1; x == "a40" }
puts "index             turns=#{n} answer=#{r.inspect}"

# rindex takes its bound once and counts down, so a collection mid-walk shows
# up in the elements it reads back rather than in the turn count.
n = 0
r = make_array.rindex { |x| churn; n += 1; x == "a1" }
puts "rindex            turns=#{n} answer=#{r.inspect}"

n = 0
r = make_array.count { |x| churn; n += 1; x.size > 1 }
puts "count             turns=#{n} answer=#{r}"

# --- the two that also build a result array ---

n = 0
r = make_array.take_while { |x| churn; n += 1; x.size > 1 }
puts "take_while        turns=#{n} answer=#{r.size} last=#{r[-1].inspect}"

# The drop_while arms print no turn count: Spinel runs the block for every
# element where CRuby stops at the first falsy one, a pre-existing difference
# this change does not touch. The kept suffix says whether the walk finished.
r = make_array.drop_while { |x| churn; x != "a11" }
puts "drop_while        answer=#{r.size} first=#{r[0].inspect} last=#{r[-1].inspect}"

# --- the in-place ones, which write back through the hoist ---

n = 0
r = make_array.map! { |x| churn; n += 1; x + "!" }
puts "map!              turns=#{n} answer=#{r.size} last=#{r[-1].inspect}"

n = 0
r = make_array.collect! { |x| churn; n += 1; x + "?" }
puts "collect!          turns=#{n} answer=#{r.size} last=#{r[-1].inspect}"

# --- the same loops with a mixed-element receiver, which takes the poly arms ---

n = 0
r = make_mixed.count { |x| churn; n += 1; x.to_s.size > 1 }
puts "poly count        turns=#{n} answer=#{r}"

n = 0
r = make_mixed.take_while { |x| churn; n += 1; !x.nil? }
puts "poly take_while   turns=#{n} answer=#{r.size} last=#{r[-1].inspect}"

r = make_mixed.drop_while { |x| churn; x != "a12" }
puts "poly drop_while   answer=#{r.size} first=#{r[0].inspect} last=#{r[-1].inspect}"

n = 0
r = make_mixed.map! { |x| churn; n += 1; x }
puts "poly map!         turns=#{n} answer=#{r.size} last=#{r[-1].inspect}"

n = 0
r = make_mixed.collect! { |x| churn; n += 1; x }
puts "poly collect!     turns=#{n} answer=#{r.size} last=#{r[-1].inspect}"

# --- a Hash receiver reaches the same two array loops through its pairs ---

n = 0
r = make_hash.take_while { |k, _v| churn; n += 1; k.size > 1 }
puts "hash take_while   turns=#{n} answer=#{r.size}"

r = make_hash.drop_while { |k, _v| churn; k != "k11" }
puts "hash drop_while   answer=#{r.size} first=#{r[0].inspect}"

# --- fill, which writes into a receiver its block never mentions ---

r = make_array.fill { |i| churn; "f#{i}" }
puts "fill              answer=#{r.size} first=#{r[0].inspect} last=#{r[-1].inspect}"

r = make_array.fill(2, 30) { |i| churn; "f#{i}" }
puts "fill span         answer=#{r.size} first=#{r[0].inspect} last=#{r[-1].inspect}"

r = make_mixed.fill { |i| churn; i }
puts "poly fill         answer=#{r.size} first=#{r[0].inspect} last=#{r[-1].inspect}"

# --- the halving searches, which read one element per turn out of the hoist ---

n = 0
r = make_sorted.bsearch { |x| churn_hard; n += 1; x >= "z000100" }
puts "bsearch           turns=#{n} answer=#{r.inspect}"

n = 0
r = make_sorted.bsearch_index { |x| churn_hard; n += 1; x >= "z000100" }
puts "bsearch_index     turns=#{n} answer=#{r.inspect}"

# bsearch lifts its running answer out of the receiver and holds it while the
# search narrows, so rooting the receiver is not enough on its own: the turn
# after the capture drops that element from the array, and nothing else holds
# it. One arm per element kind, because they root through different macros.

a = make_sorted
n = 0
r = a.bsearch { |x| n += 1; a[99] = "z%06d" % 100; churn_young if n >= 11; x >= "z000100" }
puts "bsearch answer    turns=#{n} answer=#{r.inspect}"

a = []
(1..4096).each { |i| a << ("z%06d" % i) }
n = 0
r = a.bsearch { |x| n += 1; a[99] = "z%06d" % 100; churn_young if n >= 11; x >= "z000100" }
puts "poly bsearch answ turns=#{n} answer=#{r.inspect}"

# --- controls: a receiver held in a local was always safe, because the local
# --- is itself a root. These are here to say so.

n = 0
held = make_array
r = held.find { |x| churn; n += 1; x == "a40" }
puts "held find         turns=#{n} answer=#{r.inspect}"

n = 0
held = make_array
r = held.map! { |x| churn; n += 1; x + "!" }
puts "held map!         turns=#{n} answer=#{r.size} last=#{r[-1].inspect}"

held = make_array
r = held.fill { |i| churn; "f#{i}" }
puts "held fill         answer=#{r.size} first=#{r[0].inspect} last=#{r[-1].inspect}"

n = 0
held = make_sorted
r = held.bsearch { |x| churn; n += 1; x >= "z000100" }
puts "held bsearch      turns=#{n} answer=#{r.inspect}"
