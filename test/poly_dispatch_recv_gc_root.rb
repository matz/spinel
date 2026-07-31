# The receiver temporary of a polymorphic send (the `switch (cls_id)`
# dispatch) must be GC-rooted: it can be the only live reference to the
# receiver, methods do not root their own self, and a collection triggered
# by allocation inside the callee then frees the receiver while its method
# is still running (#3476). Each go/add below forces a GC before touching
# an ivar, so an unrooted dispatch temp is collected deterministically.
class Holder
  def initialize(params = {})
    @params = params
  end

  def go
    GC.start                # collect while the dispatch temp is the only ref
    (@params[:k] || []).length
  end

  def add(x)
    GC.start
    (@params[:k] || []).length + x
  end
end

class Other
  def go
    0
  end

  def add(x)
    x
  end
end

def pick
  return Other.new if false  # gives the call site a union receiver type
  Holder.new(k: [1, 2, 3])
end

r = 0
i = 0
while i < 50
  r += pick.go               # receiver lives only as the dispatch temporary
  r += pick.add(10)          # same, through the with-args dispatch
  i += 1
end
puts "ok r=#{r}"
