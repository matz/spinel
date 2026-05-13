# #470: `sub.fetch(k, d)` where sub is sp_RbVal (came from a
# StrPolyHash lookup). Pre-fix the poly-recv `.fetch` dispatch
# enumerated only user-class arms (any class with `def fetch`)
# and silently returned the default for any Hash-storage receiver
# at runtime. Sibling to #456, which added Hash arms to poly-recv
# `[]` / `[]=` but not to `fetch`.
#
# A user-defined class with a `fetch` method is needed to force
# the call site through the polymorphic dispatch (which is what
# missed the Hash arms); without it the call site never picks the
# poly path because no arm contributes.

class FlashLike
  def fetch(key, default)
    "from-flash"
  end
end

# Keep FlashLike#fetch alive so the user-class arm is emitted.
puts FlashLike.new.fetch("x", "y")

def get_raw(h)
  h["x"]
end

def lookup(h)
  sub = get_raw(h)
  sub.fetch("title", "DEFAULT")
end

puts lookup({ "x" => { "title" => "real" } })
