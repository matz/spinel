# fetch(i) (the 1-argument, KeyError-on-miss form) on a yield receiver:
# always the element kind, never argument-dependent. The 2-argument
# with-default form is a separate row (fetch's default's type can disagree
# with the element kind) and stays declined -- see
# yield_receiver_builtin_fetch_default_declined.rb.
def t = yield.fetch(0)
p t { [1, 2, 3] }
p t { ["a", "b", "c"] }
