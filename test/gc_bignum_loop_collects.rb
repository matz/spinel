# A loop whose only allocations are a Bignum operation's: each round's
# Bignums are garbage by the next, and the loop collects them at its
# back-edge, so its memory does not grow with its round count (#11106).
# gc-nogc-poll-test runs it with SPINEL_GC_STATS=1 and counts collections.
m = 2**70
s = m + 7
(ARGV[0] || 200000).to_i.times { s = (s * 75 + 74) % m }
puts s % 1000
