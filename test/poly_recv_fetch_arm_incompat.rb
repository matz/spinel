# Sibling to poly_recv_fetch_hash_arms.rb. The dispatch arm for a
# user class whose `fetch` is sym-keyed (pinned by sym-only direct
# callers) should be SUPPRESSED at the dispatch site when the source
# call passes a String key — the String can't be cast to mrb_int sym,
# and at runtime the receiver in any source-level-valid path is a
# Hash, never the user class.
#
# Pre-fix: spinel emitted the FlashLike arm with String args going
# into a mrb_int slot — fails C compile.
# Post-fix: arm suppressed; Hash arms (added in the parent fix) cover
# the actual runtime path.

class FlashLike
  def fetch(key, default)
    @last = key
    default
  end
end

# Sym-only direct callers commit FlashLike#fetch's `key` to sym
# (mrb_int) in isolation. The dispatch site below mustn't widen it.
def warmup
  f = FlashLike.new
  f.fetch(:a, "1")
  f.fetch(:b, "2")
end

warmup

def get_raw(h); h["x"]; end

def lookup(h)
  sub = get_raw(h)
  sub.fetch("title", "")
end

puts lookup({ "x" => { "title" => "real" } })
