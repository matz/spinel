# x&. on a nil-defaulted parameter that other calls give a Regexp, a Proc or
# an Exception: the call that omits it answers nil without running the call.
def label(x = nil)
  x&.then { "set" } || "unset"
end
puts label(/a/)
puts label

def src(x = nil) = x&.source
p src(/a/)
p src
def lp(x = nil)
  x&.then { "set" } || "unset"
end
puts lp(proc { 1 })
puts lp
def le(x = nil)
  x&.then { "set" } || "unset"
end
puts le(RuntimeError.new("m"))
puts le
def lm(x = nil) = x&.message
p lm(RuntimeError.new("m")), lm
def lc(x = nil) = x&.call
p lc(-> { 5 }), lc
